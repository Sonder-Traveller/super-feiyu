// src/main.cpp
// runtime：装配所有模块，按配置启动通道线程，跑主循环。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

import fish.core;
import fish.log;
import fish.config;
import fish.capability;
import fish.probe;
import fish.memory;
import fish.media;
import fish.image;
import fish.session;
import fish.tool;
import fish.guard;
import fish.meme;
import fish.persona;
import fish.api;
import fish.control;
import fish.channels;
import fish.dispatcher;
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

  // 上游能力探测器：读缓存或现场探测（默认 30 天 TTL）
  fish::probe::Config probe_cfg;
  probe_cfg.cache_path = cfg.get_or("probe.cache_path", "data/probe/capabilities.json");
  probe_cfg.ttl_days = static_cast<int>(cfg.get_int_or("probe.ttl_days", 30));
  probe_cfg.model = chat_cfg.model;
  probe_cfg.probe_on_start = cfg.get_bool_or("probe.on_start", true);
  fish::probe::CapabilityProbe caps_probe(probe_cfg, chat_cfg.base_url, chat_cfg.api_key);

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

    // 人格文件必须存在；约束文件可选（不配就只用人格文件）
    {
      const auto pp = cfg.get_or("persona.persona_file", "persona/persona.md");
      if (!std::ifstream(pp)) problems.push_back("人格文件不存在: " + pp);
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
    // ── 上游能力探测 ──
    // 不用配置开关：上游会静默变更行为（如 WorkBuddy 在 HTTP 200 下丢弃
    // assistant 图片）。探测结果写盘缓存，默认 30 天有效，过期自动重探。
    auto caps = caps_probe.load_or_probe();
    auto effective_cfg = chat_cfg;
    effective_cfg.supports_assistant_image = caps.assistant_image;

    auto c = fish::capability::make_chat(effective_cfg);
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

  // ── 生图 + 形象库 ──
  std::unique_ptr<fish::core::IImageModel> image_model;
  const auto image_cfg = fish::capability::load_image_config(cfg);
  const auto search_cfg = fish::capability::load_search_config(cfg);
  if (!stub_mode && image_cfg.provider != "none") {
    auto im = fish::capability::make_image(image_cfg);
    if (im.ok()) {
      image_model = std::move(*im.value);
      fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "image_provider")
          .field("provider", image_cfg.provider)
          .field("model", image_cfg.model);
    } else {
      fish::log::log_line(fish::log::LogLevel::kWarn, "runtime", "image_init_failed")
          .field("detail", im.error.detail);
    }
  }

  // 形象库：目录默认 data/memory/portraits
  fish::image::PortraitStore portraits(
      cfg.get_or("memory.portraits_dir", "data/memory/portraits"));
  {
    auto e = portraits.init();
    if (!e.ok()) {
      fish::log::log_line(fish::log::LogLevel::kWarn, "runtime", "portraits_init_failed")
          .field("detail", e.detail);
    } else {
      // 用户可能直接往目录里丢图，扫描一次自动纳入
      const int added = portraits.scan_directory();
      if (added > 0) {
        fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "portraits_scanned")
            .field("added", static_cast<long long>(added));
      }
    }
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
  //
  // 两级：全局默认 + 每会话覆盖。
  //   全局默认的人格文本来自 persona.persona_file（首次启动时复制到
  //   data/persona/default.md，之后用户直接改那个文件即可）。
  //   会话覆盖由 PersonaStore 管，存 data/persona/sessions/<hash>.md。
  std::unique_ptr<fish::core::IPersona> persona_holder;
  std::unique_ptr<fish::persona::PersonaStore> persona_store;
  {
    fish::persona::PersonaStore::Config pc;
    pc.dir = cfg.get_or("persona.store_dir", "data/persona");
    persona_store = std::make_unique<fish::persona::PersonaStore>(pc);
    auto pe = persona_store->init();
    if (!pe.ok()) {
      fish::log::log_line(fish::log::LogLevel::kWarn, "runtime", "persona_store_failed")
          .field("detail", pe.detail);
      persona_store.reset();
    }

    // 全局默认人格：data/persona/default.md 优先，没有就从配置指定的文件播种一次。
    bool seeded = false;
    if (persona_store != nullptr) {
      auto cur = persona_store->default_persona();
      if (cur.ok() && cur.value->empty()) {
        const auto src = cfg.get_or("persona.persona_file", "persona/persona.md");
        if (auto seed = fish::persona::read_text_file(src); seed.ok() && !seed.value->empty()) {
          if (auto e = persona_store->set_default_persona(*seed.value); e.ok()) {
            seeded = true;
            fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "persona_seeded")
                .field("from", src)
                .field("bytes", static_cast<long long>(seed.value->size()));
          }
        }
      }
    }

    // LayeredPersona 仍要存在：dynamic_suffix（时间/场景）由它提供。
    // 它的 stable_prefix 只在 PersonaStore 不可用时兜底。
    auto pr = fish::orchestrator::load_persona(
        cfg.get_or("persona.persona_file", "persona/persona.md"),
        cfg.get_or("persona.rules_file", ""));
    if (pr.ok()) {
      persona_holder =
          std::make_unique<fish::orchestrator::LayeredPersona>(std::move(*pr.value));
    } else {
      // 人格文件可以不存在（默认就是空的），这里只记一条日志，不阻断启动。
      fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "persona_empty")
          .field("detail", pr.error.detail);
      persona_holder = std::make_unique<fish::orchestrator::LayeredPersona>("", "");
    }
  }

  // ── 记忆 ──
  fish::memory::MemorySystem::Config mem_cfg;
  mem_cfg.model_context_tokens =
      static_cast<int>(cfg.get_int_or("memory.model_context_tokens", 131072));
  mem_cfg.raw_context_ratio =
      cfg.get_double_or("memory.raw_context_ratio", 0.40);   // 硬上限 50%
  mem_cfg.summary_ratio = cfg.get_double_or("memory.summary_ratio", 0.25);
  mem_cfg.notes_dir = cfg.get_or("memory.notes_dir", "data/memory/notes");
  mem_cfg.db_path = cfg.get_or("memory.db_path", "data/memory/index.db");

  fish::memory::MemorySystem memory_sys(mem_cfg, embedding.get());
  bool memory_ready = false;
  {
    auto e = memory_sys.init();
    if (!e.ok()) {
      fish::log::log_line(fish::log::LogLevel::kError, "runtime", "memory_init_failed")
          .field("detail", e.detail);
      if (!opt.self_test) return 1;
    } else {
      memory_ready = true;
    }
  }

  fish::memory::Consolidator consolidator(chat.get(), &memory_sys.notes(),
                                          &memory_sys.index(), embedding.get());

  fish::orchestrator::ReplyBudget budget(
      static_cast<int>(cfg.get_int_or("orchestrator.max_replies", 4)));

  // 媒体缓存：入站图片/语音落本地副本。
  // 平台给的 URL 是临时签名链接，过期成死链，记忆里必须存本地路径。
  fish::media::MediaCache::Config media_cfg;
  media_cfg.dir = cfg.get_or("memory.media_dir", "data/memory/media");
  fish::media::MediaCache media_cache(media_cfg);
  {
    auto e = media_cache.init();
    if (!e.ok()) {
      fish::log::log_line(fish::log::LogLevel::kWarn, "runtime", "media_init_failed")
          .field("detail", e.detail);
    }
  }

  // ── 工具注册 ──
  // 工具是「能力」，模型据此决定何时调用。没有对应后端时就不注册，
  // 模型也不会去尝试调用不存在的工具。
  fish::tool::Registry tools;
  std::unique_ptr<fish::core::IWebSearch> search_holder;
  std::unique_ptr<fish::core::IWebFetch> fetch_holder;
  std::unique_ptr<fish::meme::MemeStore> meme_store;

  // 记忆检索：用向量 + 关键词混合召回
  if (memory_ready) {
    tools.add(std::make_shared<fish::tool::MemorySearchTool>(
        [&memory_sys](const std::string& query, int top_k) {
          std::vector<std::string> out;
          auto hits = memory_sys.search(query, top_k);
          if (hits.ok()) {
            for (const auto& h : *hits.value) out.push_back(h.text);
          }
          return out;
        }));
  }

  // 本地图片检索：按标签在形象库里找
  {
    tools.add(std::make_shared<fish::tool::ImageSearchTool>(
      [&portraits](const std::string& query) {
        std::vector<fish::tool::ImageSearchTool::Hit> out;
        for (const auto& p : portraits.find_by_tag(query)) {
          out.push_back({p.path, p.caption});
        }
        return out;
      }));
  }

  // 语音发送：把模型给的一句话交给 TTS 合成，mp3 路径由工具交给管线。
  // TTS 未启用时工具不可注册——模型看不到用不了，也不会白调。
  if (tts != nullptr) {
    tools.add(std::make_shared<fish::tool::SendVoiceTool>(
        [tts_ptr = tts.get()](const std::string& text) -> std::string {
          auto audio = tts_ptr->synthesize(text);
          if (!audio.ok()) {
            fish::log::log_line(fish::log::LogLevel::kWarn, "tool", "tts_failed")
                .field("detail", audio.error.detail);
            return {};
          }
          return *audio.value;
        }));
  }

  // 网络热梗：本地库优先，未命中时可选联网并写回
  {
    fish::meme::MemeStore::Config meme_cfg;
    meme_cfg.dir = cfg.get_or("meme.dir", "data/memes");
    meme_cfg.max_results = static_cast<int>(cfg.get_int_or("meme.max_results", 5));
    meme_store = std::make_unique<fish::meme::MemeStore>(meme_cfg);
    auto e = meme_store->init();
    if (!e.ok()) {
      fish::log::log_line(fish::log::LogLevel::kWarn, "runtime", "meme_init_failed")
          .field("detail", e.detail);
      meme_store.reset();
    }
  }

  // 网络搜索：只有配置了 provider 才注册
  if (!stub_mode && search_cfg.provider != "none" && !search_cfg.api_key.empty()) {
    auto searcher = fish::capability::make_search(search_cfg);
    if (searcher.ok() && *searcher.value) {
      search_holder = std::move(*searcher.value);
      auto* raw = search_holder.get();
      tools.add(std::make_shared<fish::tool::WebSearchTool>(
          [raw](const std::string& query, int max_results) {
            std::vector<fish::tool::WebSearchTool::Hit> out;
            auto r = raw->search(query, max_results);
            if (r.ok()) {
              for (const auto& h : *r.value) {
                out.push_back({h.title, h.url, h.snippet});
              }
            }
            return out;
          }));
    }
  }

  // 网页抓取：与搜索配套，search 找链接、fetch 读内容
  if (!stub_mode && search_cfg.provider != "none" && !search_cfg.api_key.empty()) {
    auto fetcher = fish::capability::make_fetch(search_cfg);
    if (fetcher.ok() && *fetcher.value) {
      fetch_holder = std::move(*fetcher.value);
      auto* raw = fetch_holder.get();
      tools.add(std::make_shared<fish::tool::WebFetchTool>(
          [raw](const std::string& url) {
            fish::tool::WebFetchTool::Page p;
            auto r = raw->fetch(url);
            if (r.ok()) {
              p.url = r.value->url;
              p.title = r.value->title;
              p.content = r.value->content;
            }
            return p;
          }));
    }
  }

  // 热梗查询：本地优先，联网作为可选补充
  if (meme_store) {
    auto* ms = meme_store.get();
    auto* raw_search = search_holder.get();

    fish::tool::MemeLookupTool::RemoteSearcher remote;
    if (raw_search != nullptr) {
      // 有搜索后端时才启用联网补充
      remote = [raw_search](const std::string& query) {
        std::vector<fish::tool::MemeLookupTool::Hit> out;
        auto r = raw_search->search(query + " 梗 含义", 3);
        if (r.ok()) {
          for (const auto& h : *r.value) {
            fish::tool::MemeLookupTool::Hit hit;
            hit.term = query;
            // 用搜索结果拼一个粗略解释
            hit.meaning = h.title + (h.snippet.empty() ? "" : "。" + h.snippet);
            hit.usage = h.url;
            out.push_back(std::move(hit));
          }
        }
        return out;
      };
    }

    tools.add(std::make_shared<fish::tool::MemeLookupTool>(
        [ms](const std::string& query) {
          std::vector<fish::tool::MemeLookupTool::Hit> out;
          for (const auto& e : ms->search(query)) {
            out.push_back({e.term, e.meaning, e.usage});
          }
          return out;
        },
        remote,
        [ms](const std::vector<fish::tool::MemeLookupTool::Hit>& hits) {
          std::vector<fish::meme::Entry> entries;
          for (const auto& h : hits) {
            fish::meme::Entry e;
            e.term = h.term;
            e.meaning = h.meaning;
            e.usage = h.usage;
            e.origin = "web";
            entries.push_back(std::move(e));
          }
          return ms->import_many(entries);
        }));
  }

  fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "tools_ready")
      .field("count", static_cast<long long>(tools.size()));

  // ── 安全防护 ──
  fish::guard::InputGuard::Config guard_in_cfg;
  guard_in_cfg.max_bytes =
      static_cast<std::size_t>(cfg.get_int_or("guard.max_input_bytes", 8000));
  guard_in_cfg.detect_injection = cfg.get_bool_or("guard.detect_injection", true);
  guard_in_cfg.reject_injection = cfg.get_bool_or("guard.reject_injection", true);
  fish::guard::InputGuard input_guard(guard_in_cfg);

  fish::guard::OutputGuard::Config guard_out_cfg;
  guard_out_cfg.max_bytes =
      static_cast<std::size_t>(cfg.get_int_or("guard.max_output_bytes", 4000));
  guard_out_cfg.redact_secrets = cfg.get_bool_or("guard.redact_secrets", true);
  guard_out_cfg.strip_links = cfg.get_bool_or("guard.strip_links", true);
  fish::guard::OutputGuard output_guard(guard_out_cfg);

  fish::guard::RateLimiter::Config rate_cfg;
  rate_cfg.max_per_window = static_cast<int>(cfg.get_int_or("guard.rate_limit", 12));
  rate_cfg.window_seconds = static_cast<int>(cfg.get_int_or("guard.rate_window_s", 60));
  fish::guard::RateLimiter rate_limiter(rate_cfg);

  fish::orchestrator::Pipeline::Deps deps;
  deps.chat = chat.get();
  deps.embedding = embedding.get();
  deps.persona = persona_holder.get();
  deps.personas = persona_store.get();
  deps.budget = &budget;
  deps.tts = tts.get();
  deps.stt = stt.get();
  deps.image = image_model.get();
  deps.portraits = &portraits;
  deps.media = &media_cache;
  deps.tools = &tools;
  deps.max_tool_rounds = static_cast<int>(cfg.get_int_or("tool.max_rounds", 3));
  deps.input_guard = &input_guard;
  deps.output_guard = &output_guard;
  deps.rate_limiter = &rate_limiter;
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

  // ── 每通道收消息统计 ──
  // 声明提前到 API 启动之前：/v1/status 要读它，回调捕获的是指针表，
  // 通道线程在后面才往里填。
  std::vector<std::unique_ptr<ChannelStats>> stats;
  stats.reserve(channels.size());
  for (std::size_t i = 0; i < channels.size(); ++i) {
    stats.push_back(std::make_unique<ChannelStats>());
  }

  // ── 控制面 API ──
  //
  // 只暴露 API，不带 UI，供另一个进程管理人格 / 配置 / 扫码登录。
  // 默认不启动（port=0），要开就在配置里写：
  //   [api] port = 8765
  // token 从环境变量读（FEIYU_API_TOKEN），不写进配置和代码。
  std::unique_ptr<fish::control::ControlService> control;
  std::unique_ptr<fish::api::Server> api_server;
  {
    const auto port = static_cast<int>(cfg.get_int_or("api.port", 0));
    if (port > 0) {
      fish::api::Server::Config ac;
      ac.bind_addr = cfg.get_or("api.bind", "127.0.0.1");
      ac.port = port;

      // 鉴权：默认开。token 必须从环境变量来。
      ac.auth.enabled = cfg.get_bool_or("api.auth", true);
      if (const char* tok = std::getenv("FEIYU_API_TOKEN"); tok != nullptr) {
        ac.auth.token = tok;
      }
      if (ac.auth.enabled && ac.auth.token.empty()) {
        fish::log::log_line(fish::log::LogLevel::kError, "api", "token_missing")
            .field("hint", "设置 FEIYU_API_TOKEN 或把 api.auth 设为 false");
        // 不静默降级成无鉴权：宁可不起，也不能裸奔
        api_server.reset();
      } else {
        // 控制面要能改 config.toml 和 .env.local，所以必须知道它们的真实路径。
        // .env.local 与 config.toml 同目录（部署脚本保证这一点）。
        fish::control::ControlService::Config cc;
        cc.config_path = opt.config_path;
        cc.env_path = (std::filesystem::path(opt.config_path).parent_path() /
                       ".env.local")
                          .string();
        control = std::make_unique<fish::control::ControlService>(cc);

        api_server = std::make_unique<fish::api::Server>(ac, persona_store.get(),
                                                         control.get());
        // /v1/restart：优雅退出自己，systemd Restart=always 拉起新进程。
        // 扫码绑定/改配置后凭据要重启才生效，GUI 调这个接口即可。
        api_server->set_restart_callback([]() {
          ::fish::log::log_line(fish::log::LogLevel::kInfo, "api",
                                "restart_requested");
          std::raise(SIGTERM);
        });
        // /v1/status 的通道信息：用回调注入，api 模块不需要认识 ChannelStats
        api_server->set_status_provider([&stats, &channel_names]() {
          std::string out = "[";
          for (std::size_t i = 0; i < channel_names.size(); ++i) {
            if (i > 0) out += ",";
            out += R"({"name":")" + channel_names[i] + R"(","connected":true,"received":)";
            out += std::to_string(i < stats.size() ? stats[i]->polled.load() : 0);
            out += R"(,"errors":)";
            out += std::to_string(i < stats.size() ? stats[i]->errors.load() : 0);
            out += "}";
          }
          out += "]";
          return out;
        });

        auto ae = api_server->start();
        if (!ae.ok()) {
          fish::log::log_line(fish::log::LogLevel::kError, "api", "start_failed")
              .field("detail", ae.detail);
          api_server.reset();
        }
      }
    }
  }

  // ── 调度器：通道线程只 poll，处理在线程池里跑 ──
  // 这样慢的模型调用不会阻塞收消息，多会话可真正并行。
  fish::session::SessionRegistry sessions;
  // 线程数从配置读，0 表示自动（min(核数, 8)，至少 2）
  fish::dispatcher::Dispatcher dispatcher(
      {.threads = static_cast<std::size_t>(cfg.get_int_or("dispatcher.threads", 0))});

  {
    // handler 捕获各通道指针，按 session 的 channel 名找到对应通道
    std::map<std::string, fish::core::IChannel*> by_name;
    for (std::size_t i = 0; i < channels.size(); ++i) {
      by_name[channel_names[i]] = channels[i].get();
    }
    dispatcher.set_handler([&pipeline, by_name](fish::dispatcher::Task& task) -> int {
      auto it = by_name.find(task.channel_name);
      if (it == by_name.end() || it->second == nullptr) return -1;
      auto r = pipeline.handle(*it->second, task.msg);
      if (!r.ok()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "runtime", "handle_failed")
            .field("session", task.session ? task.session->key() : "?")
            .field("detail", r.error.detail);
        return -1;
      }
      if (!r.value.has_value()) return 0;
      return *r.value;
    });
  }

  // ── 每通道一个收消息线程 ──
  std::vector<std::thread> workers;

  for (std::size_t i = 0; i < channels.size(); ++i) {
    auto* st_ptr = stats[i].get();

    fish::core::IChannel* raw = channels[i].get();
    const std::string name = channel_names[i];

    workers.emplace_back([raw, st_ptr, &dispatcher, &sessions, name] {
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

        // 构造会话上下文：key 由统一函数推导，保证落盘和内存用同一把键
        const auto now_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        fish::session::SessionInfo info;
        info.key = fish::session::session_key(*msg.value);
        info.channel = name;
        info.kind = msg.value->kind;
        info.created_at_ms = now_ms;
        info.last_active_ms = now_ms;

        fish::dispatcher::Task task;
        task.msg = std::move(*msg.value);
        task.session = sessions.get_or_create(info);
        task.channel_name = name;

        if (!dispatcher.submit(std::move(task))) {
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
  // 通道线程停了之后，把已提交的任务跑完再退出
  // （否则进行中的模型调用会被硬中断，回复丢失）
  dispatcher.shutdown();
  channels.clear();

  fish::log::log_line(fish::log::LogLevel::kInfo, "runtime", "shutdown");
  return 0;
}



