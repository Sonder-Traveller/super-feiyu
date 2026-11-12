// src/main.cpp
// runtime：装配所有模块，跑主循环。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

import fish.core;
import fish.log;
import fish.config;
import fish.capability;
import fish.memory;
import fish.transport;
import fish.orchestrator;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

// 确定性桩：无 API key 时用于验收，保证流程可跑通
class EchoChat final : public fish::core::IChatModel {
 public:
  fish::core::Result<Response> complete(const Request& req) override {
    std::string user_text;
    for (const auto& b : req.user_blocks) {
      if (b.kind == fish::core::BlockKind::kText) user_text += b.text;
    }
    Response r;
    r.text = "（桩模式）你说：" + user_text + " ｜ 我记得 " +
             std::to_string(req.history.size()) + " 条上下文";
    r.prompt_tokens = static_cast<int>(req.system_prompt.size() / 4 + user_text.size() / 4);
    r.cached_tokens = static_cast<int>(req.system_prompt.size() / 4);
    r.output_tokens = static_cast<int>(r.text.size() / 4);
    return fish::core::Result<Response>::ok(std::move(r));
  }
};

class ZeroEmbedding final : public fish::core::IEmbeddingModel {
 public:
  int dimension() const noexcept override { return 1024; }
  fish::core::Result<std::vector<float>> embed(const std::string& text) override {
    std::vector<float> v(1024, 0.0F);
    std::size_t h = 1469598103934665603ULL;
    for (const char c : text) {
      h ^= static_cast<unsigned char>(c);
      h *= 1099511628211ULL;
    }
    for (int i = 0; i < 1024; ++i) {
      v[static_cast<std::size_t>(i)] = static_cast<float>((h >> (i % 32)) & 0xFF) / 255.0F;
    }
    return fish::core::Result<std::vector<float>>::ok(std::move(v));
  }
};

struct Options {
  std::string config_path{"config.toml"};
  bool self_test{false};
  bool stub{false};
  bool live_test{false};
};

Options parse_args(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      opt.config_path = argv[++i];
    } else if (a == "--self-test") {
      opt.self_test = true;
    } else if (a == "--stub") {
      opt.stub = true;
    } else if (a == "--live-test") {
      opt.live_test = true;
    }
  }
  return opt;
}

}  // namespace

int main(int argc, char** argv) {
  const auto opt = parse_args(argc, argv);
  fish::log::Logger::instance().set_level(fish::log::LogLevel::kInfo);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  fish::capability::HttpClient::global_init();
  struct Guard {
    ~Guard() { fish::capability::HttpClient::global_cleanup(); }
  } guard;

  // ── 配置 ──
  fish::config::Config cfg;
  {
    auto loaded = fish::config::Config::load(opt.config_path);
    if (loaded.ok()) {
      cfg = std::move(*loaded.value);
    } else if (!opt.self_test) {
      fish::log::log_line(fish::log::LogLevel::kError, "runtime", "config_load_failed")
          .field("detail", loaded.error.detail);
      return 1;
    } else {
      fish::log::log_line(fish::log::LogLevel::kWarn, "runtime",
                          "config_missing_using_defaults");
    }
  }

  // ── 能力 ──
  const auto chat_cfg = fish::capability::load_chat_config(cfg);
  const auto emb_cfg = fish::capability::load_embedding_config(cfg);
  const auto stt_cfg = fish::capability::load_stt_config(cfg);
  const auto tts_cfg = fish::capability::load_tts_config(cfg);

  std::unique_ptr<fish::core::IChatModel> chat;
  std::unique_ptr<fish::core::IEmbeddingModel> embedding;
  std::unique_ptr<fish::core::ISpeechToText> stt;
  std::unique_ptr<fish::core::ITextToSpeech> tts;

  const bool stub_mode = opt.stub || (opt.self_test && !opt.live_test);

  if (!stub_mode && !chat_cfg.api_key.empty()) {
    auto c = fish::capability::make_chat(chat_cfg);
    if (c.ok()) {
      chat = std::move(*c.value);
      fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "chat_provider")
          .field("provider", chat_cfg.provider)
          .field("model", chat_cfg.model);
    } else {
      fish::log::log_line(fish::log::LogLevel::kError, "runtime", "chat_init_failed")
          .field("detail", c.error.detail);
      return 1;
    }
  }
  if (!stub_mode && !emb_cfg.api_key.empty()) {
    auto e = fish::capability::make_embedding(emb_cfg);
    if (e.ok()) {
      embedding = std::move(*e.value);
      fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "embedding_provider")
          .field("provider", emb_cfg.provider);
    }
  }
  if (!stub_mode && tts_cfg.provider != "none") {
    auto t = fish::capability::make_tts(tts_cfg);
    if (t.ok()) tts = std::move(*t.value);
  }
  if (!stub_mode && stt_cfg.provider != "none" && !stt_cfg.api_key.empty()) {
    auto s = fish::capability::make_stt(stt_cfg);
    if (s.ok()) stt = std::move(*s.value);
  }

  if (!chat) {
    chat = std::make_unique<EchoChat>();
    fish::log::log_line(fish::log::LogLevel::kWarn, "runtime", "chat_stub_mode");
  }
  if (!embedding) {
    embedding = std::make_unique<ZeroEmbedding>();
    fish::log::log_line(fish::log::LogLevel::kWarn, "runtime", "embedding_stub_mode");
  }

  // ── 人格 ──
  std::unique_ptr<fish::core::IPersona> persona_holder;
  {
    auto pr = fish::orchestrator::load_persona(
        cfg.get_or("persona.persona_file", "persona/persona.md"),
        cfg.get_or("persona.rules_file", "persona/rules.md"));
    if (pr.ok()) {
      persona_holder = std::make_unique<fish::orchestrator::LayeredPersona>(
          std::move(*pr.value));
    } else {
      fish::log::log_line(fish::log::LogLevel::kError, "runtime", "persona_load_failed")
          .field("detail", pr.error.detail);
      if (!opt.self_test) return 1;
      persona_holder = std::make_unique<fish::orchestrator::LayeredPersona>(
          "你叫大肥鱼，说话简短自然。", "不要主动开启新话题。");
    }
  }

  // ── 记忆 ──
  fish::memory::MemorySystem::Config mem_cfg;
  mem_cfg.working_rounds = static_cast<int>(cfg.get_int_or("memory.working_rounds", 20));
  mem_cfg.consolidate_every =
      static_cast<int>(cfg.get_int_or("memory.consolidate_every", 20));
  mem_cfg.recall_top_k = static_cast<int>(cfg.get_int_or("memory.recall_top_k", 5));
  mem_cfg.episode_top_k = static_cast<int>(cfg.get_int_or("memory.episode_top_k", 3));
  mem_cfg.notes_dir = cfg.get_or("memory.notes_dir", "data/memory/notes");
  mem_cfg.db_path = cfg.get_or("memory.db_path", "data/memory/index.db");

  fish::memory::MemorySystem memory_sys(mem_cfg, embedding.get());
  {
    auto e = memory_sys.init();
    if (!e.ok()) {
      fish::log::log_line(fish::log::LogLevel::kError, "runtime", "memory_init_failed")
          .field("detail", e.detail);
      if (!opt.self_test) return 1;
    }
  }

  fish::memory::Consolidator consolidator(chat.get(), &memory_sys.notes(),
                                          &memory_sys.index(), embedding.get());

  // ── 预算与管线 ──
  fish::orchestrator::ReplyBudget budget(
      static_cast<int>(cfg.get_int_or("orchestrator.max_replies", 4)));

  fish::orchestrator::Pipeline::Deps deps;
  deps.chat = chat.get();
  deps.embedding = embedding.get();
  deps.persona = persona_holder.get();
  deps.budget = &budget;
  deps.tts = tts.get();
  deps.stt = stt.get();
  deps.memory = &memory_sys;
  deps.consolidator = &consolidator;
  deps.group_voice = cfg.get_bool_or("capability.tts.group_voice", false);
  deps.passive_window_ms =
      static_cast<int>(cfg.get_int_or("orchestrator.passive_window_ms", 300000));

  fish::orchestrator::Pipeline pipeline(deps);

  // ── 自检 ──
  if (opt.self_test || opt.live_test) {
    fish::transport::LocalChannel ch;
    ch.inject_text("self-test-group", "在吗", true, false);
    ch.inject_text("self-test-group", "@大肥鱼 在吗", true, true);
    ch.inject_text("self-test-private", "你好", false, true);

    for (int i = 0; i < 3; ++i) {
      auto msg = ch.poll();
      if (!msg.ok()) break;
      auto r = pipeline.handle(ch, *msg.value);
      if (!r.ok()) {
        fish::log::log_line(fish::log::LogLevel::kError, "self_test", "handle_failed")
            .field("detail", r.error.detail);
        return 1;
      }
    }

    const auto sent = ch.sent_texts();
    std::printf("\n=== SELF TEST ===\n");
    std::printf("sent count: %zu (expected 2)\n", sent.size());
    for (const auto& s : sent) std::printf("  -> %s\n", s.c_str());
    std::printf("working memory rounds(group): %d\n",
                memory_sys.round_count("self-test-group"));

    if (sent.size() != 2) {
      std::printf("FAIL: expected 2 replies\n");
      return 1;
    }
    std::printf("SELF TEST PASSED\n");
    return 0;
  }

  // ── 主循环 ──
  fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "started")
      .field("config", opt.config_path);

  fish::transport::LocalChannel channel;
  while (!g_stop) {
    auto msg = channel.poll();
    if (!msg.ok()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    auto r = pipeline.handle(channel, *msg.value);
    if (!r.ok()) {
      fish::log::log_line(fish::log::LogLevel::kWarn, "runtime", "handle_failed")
          .field("detail", r.error.detail);
    }
  }

  fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "shutdown");
  return 0;
}

