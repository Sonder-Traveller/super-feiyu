// src/main.cpp
// runtime：装配所有模块，按配置启动通道线程，跑主循环。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

import fish.core;
import fish.log;
import fish.config;
import fish.capability;
import fish.memory;
import fish.channels;
import fish.orchestrator;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

// ── 桩：无 key 时保证流程可跑通 ─────────────────────────
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
  bool live_test{false};
  bool stub{false};
};

Options parse_args(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      opt.config_path = argv[++i];
    } else if (a == "--self-test") {
      opt.self_test = true;
    } else if (a == "--live-test") {
      opt.live_test = true;
    } else if (a == "--stub") {
      opt.stub = true;
    }
  }
  return opt;
}

// 统计每通道的收发计数（观测用）
struct ChannelStats {
  std::atomic<std::uint64_t> polled{0};
  std::atomic<std::uint64_t> handled{0};
  std::atomic<std::uint64_t> errors{0};
};

}  // namespace

int main(int argc, char** argv) {
  const auto opt = parse_args(argc, argv);
  fish::log::Logger::instance().set_level(fish::log::LogLevel::kInfo);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

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
      fish::log::log_line(fish::log::LogLevel::kWarn, "runtime", "config_missing");
    }
  }

  // 日志级别
  {
    const auto lv = cfg.get_or("log.level", "info");
    if (lv == "debug") fish::log::Logger::instance().set_level(fish::log::LogLevel::kDebug);
    else if (lv == "warn") fish::log::Logger::instance().set_level(fish::log::LogLevel::kWarn);
    else if (lv == "error") fish::log::Logger::instance().set_level(fish::log::LogLevel::kError);
  }

  const bool stub_mode = opt.stub || (opt.self_test && !opt.live_test);
  const auto chat_cfg = fish::capability::load_chat_config(cfg);

  // ── 启动校验：配置有错就立即失败，不允许静默运行 ──
  {
    std::vector<std::string> problems;

    const bool qq_on = cfg.get_bool_or("channel.qq_official.enabled", false);
    const bool ob_on = cfg.get_bool_or("channel.qq_onebot.enabled", false);
    const bool wx_on = cfg.get_bool_or("channel.wechat.enabled", false);

    if (qq_on) {
      if (cfg.get_or("channel.qq_official.app_id", "").empty()) {
        problems.push_back("channel.qq_official.app_id 为空");
      }
      if (cfg.get_or("channel.qq_official.app_secret", "").empty()) {
        problems.push_back("channel.qq_official.app_secret 为空（检查 QQ_APP_SECRET 环境变量）");
      }
    }
    if (ob_on) {
      if (cfg.get_or("channel.qq_onebot.ws_url", "").empty()) {
        problems.push_back("channel.qq_onebot.ws_url 为空");
      }
      if (cfg.get_or("channel.qq_onebot.self_id", "").empty()) {
        problems.push_back("channel.qq_onebot.self_id 为空（否则无法判定是否被 @）");
      }
    }
    if (wx_on && cfg.get_or("channel.wechat.bot_token", "").empty()) {
      problems.push_back("channel.wechat.bot_token 为空");
    }

    // 人格文件必须存在
    {
      const auto pp = cfg.get_or("persona.persona_file", "persona/persona.md");
      const auto rp = cfg.get_or("persona.rules_file", "persona/rules.md");
      if (!std::ifstream(pp)) problems.push_back("人格文件不存在: " + pp);
      if (!std::ifstream(rp)) problems.push_back("约束文件不存在: " + rp);
    }

    // 用了真实 provider 就必须有 key
    if (!stub_mode) {
      if (chat_cfg.api_key.empty() && chat_cfg.provider != "ollama") {
        problems.push_back("capability.chat.api_key 为空（检查 DEEPSEEK_API_KEY 环境变量）");
      }
      if (!problems.empty()) {
        for (const auto& p : problems) {
          fish::log::log_line(fish::log::LogLevel::kError, "runtime", "config_invalid")
              .field("problem", p);
        }
        std::fprintf(stderr, "\n启动失败：%zu 个配置问题，见上方日志。\n", problems.size());
        return 1;
      }
    }
  }
  // ── 能力 ──
  // (chat_cfg 已前移)
  const auto emb_cfg = fish::capability::load_embedding_config(cfg);
  const auto stt_cfg = fish::capability::load_stt_config(cfg);
  const auto tts_cfg = fish::capability::load_tts_config(cfg);

  std::unique_ptr<fish::core::IChatModel> chat;
  std::unique_ptr<fish::core::IEmbeddingModel> embedding;
  std::unique_ptr<fish::core::ISpeechToText> stt;
  std::unique_ptr<fish::core::ITextToSpeech> tts;

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
          .field("provider", emb_cfg.provider)
          .field("model", emb_cfg.model);
    } else {
      fish::log::log_line(fish::log::LogLevel::kWarn, "runtime", "embedding_init_failed")
          .field("detail", e.error.detail);
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
      persona_holder =
          std::make_unique<fish::orchestrator::LayeredPersona>(std::move(*pr.value));
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
  mem_cfg.consolidate_every = static_cast<int>(cfg.get_int_or("memory.consolidate_every", 20));
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
    fish::channels::LocalChannel ch;
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

  // ── 按配置装配通道 ──
  std::vector<std::unique_ptr<fish::core::IChannel>> channels;
  std::vector<std::string> channel_names;

  if (cfg.get_bool_or("channel.qq_official.enabled", false)) {
    fish::channels::QQOfficialConfig qc;
    qc.app_id = cfg.get_or("channel.qq_official.app_id", "");
    qc.app_secret = cfg.get_or("channel.qq_official.app_secret", "");
    auto ch = std::make_unique<fish::channels::QQOfficialChannel>(qc);
    auto e = ch->connect();
    if (e.ok()) {
      fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "channel_enabled")
          .field("channel", "qq_official");
      channels.push_back(std::move(ch));
      channel_names.push_back("qq_official");
    } else {
      fish::log::log_line(fish::log::LogLevel::kError, "runtime", "channel_connect_failed")
          .field("channel", "qq_official")
          .field("detail", e.detail);
    }
  }

  if (cfg.get_bool_or("channel.qq_onebot.enabled", false)) {
    fish::channels::OneBotConfig oc;
    oc.ws_url = cfg.get_or("channel.qq_onebot.ws_url", "ws://127.0.0.1:3001");
    oc.access_token = cfg.get_or("channel.qq_onebot.access_token", "");
    oc.self_id = cfg.get_or("channel.qq_onebot.self_id", "");
    oc.http_url = cfg.get_or("channel.qq_onebot.http_url", "");
    auto ch = std::make_unique<fish::channels::QQOneBotChannel>(oc);
    auto e = ch->connect();
    if (e.ok()) {
      fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "channel_enabled")
          .field("channel", "qq_onebot");
      channels.push_back(std::move(ch));
      channel_names.push_back("qq_onebot");
    } else {
      fish::log::log_line(fish::log::LogLevel::kError, "runtime", "channel_connect_failed")
          .field("channel", "qq_onebot")
          .field("detail", e.detail);
    }
  }

  if (cfg.get_bool_or("channel.wechat.enabled", false)) {
    fish::channels::WeChatConfig wc;
    wc.bot_token = cfg.get_or("channel.wechat.bot_token", "");
    auto ch = std::make_unique<fish::channels::WeChatIlinkChannel>(wc);
    auto e = ch->connect();
    if (e.ok()) {
      fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "channel_enabled")
          .field("channel", "wechat");
      channels.push_back(std::move(ch));
      channel_names.push_back("wechat");
    } else {
      fish::log::log_line(fish::log::LogLevel::kError, "runtime", "channel_connect_failed")
          .field("channel", "wechat")
          .field("detail", e.detail);
    }
  }

  if (channels.empty()) {
    fish::log::log_line(fish::log::LogLevel::kWarn, "runtime", "no_channel_enabled")
        .field("hint", std::string("在 config.toml 里启用 channel.qq_official / channel.qq_onebot / channel.wechat"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return 0;
  }

  fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "started")
      .field("config", opt.config_path)
      .field("channels", static_cast<long long>(channels.size()));

  // ── 每通道一个线程 ──
  std::vector<std::thread> workers;
  std::vector<std::unique_ptr<ChannelStats>> stats;
  stats.reserve(channels.size());

  for (std::size_t i = 0; i < channels.size(); ++i) {
    auto st = std::make_unique<ChannelStats>();
    auto* st_ptr = st.get();
    stats.push_back(std::move(st));

    fish::core::IChannel* raw = channels[i].get();
    const std::string name = channel_names[i];

    workers.emplace_back([raw, st_ptr, &pipeline, name] {
      while (!g_stop.load()) {
        auto msg = raw->poll();
        if (!msg.ok()) {
          // timeout 是正常情况，其他错误记录
          if (msg.error.code != fish::core::ErrorCode::kTimeout) {
            st_ptr->errors.fetch_add(1);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
        st_ptr->polled.fetch_add(1);
        auto r = pipeline.handle(*raw, *msg.value);
        if (r.ok()) {
          st_ptr->handled.fetch_add(1);
        } else {
          st_ptr->errors.fetch_add(1);
        }
      }
    });
  }

  // ── 主线程：等信号 + 定期打指标 + 周期维护 ──
  int tick = 0;
  int mem_ticks = 0;
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 每 60 秒：清理预算/去重表（否则长期运行会缓慢增长）
    if (++tick % 300 == 0) {
      pipeline.maintenance();
    }

    // 每 24 小时：按衰减权重淘汰旧记忆
    if (++mem_ticks % 432000 == 0 && mem_ticks > 0) {
      auto mr = memory_sys.maintain(30.0, 0.05);
      if (mr.ok() && mr.value->evicted > 0) {
        fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "memory_maintained")
            .field("evicted", mr.value->evicted)
            .field("facts", static_cast<long long>(mr.value->facts_after));
      }
    }

    if (tick % 300 == 0) {  // 每 60 秒
      std::uint64_t polled = 0;
      std::uint64_t handled = 0;
      std::uint64_t errors = 0;
      for (const auto& s : stats) {
        polled += s->polled.load();
        handled += s->handled.load();
        errors += s->errors.load();
      }
      fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "metrics")
          .field("polled", static_cast<long long>(polled))
          .field("handled", static_cast<long long>(handled))
          .field("errors", static_cast<long long>(errors));
    }
  }

  fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "stopping");
  for (auto& t : workers) {
    if (t.joinable()) t.join();
  }
  channels.clear();

  fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "shutdown");
  return 0;
}



