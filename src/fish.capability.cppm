// src/fish.capability.cppm
// 远程能力：HTTP 基础设施 + 多 provider 统一接口。
// 本机零推理，此处全是网络调用。
module;
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

export module fish.capability;

import fish.core;
import fish.log;
import fish.config;

export namespace fish::capability {

struct HttpRequest {
  std::string method{"POST"};
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  int timeout_ms{30000};
  int max_retries{2};
  bool want_binary{false};
};

struct HttpResult {
  int status{0};
  std::string body;
  std::string bytes;
  std::map<std::string, std::string> headers;
};

// 按 provider 独立熔断：一家挂掉不拖累另一家
class CircuitBreaker {
 public:
  explicit CircuitBreaker(std::string name, int threshold = 5,
                          std::chrono::seconds cooldown = std::chrono::seconds(30));
  [[nodiscard]] bool allow() const;
  void on_success();
  void on_failure();
  [[nodiscard]] bool is_open() const;

 private:
  std::string name_;
  int threshold_;
  std::chrono::seconds cooldown_;
  mutable std::mutex mu_;
  int failures_{0};
  fish::core::TimePoint open_until_{};
};

class HttpClient {
 public:
  static HttpClient& instance();
  static void global_init();
  static void global_cleanup();

  fish::core::Result<HttpResult> perform(const HttpRequest& req,
                                         CircuitBreaker* breaker = nullptr);
  fish::core::Result<HttpResult> post_json(
      const std::string& url, const std::map<std::string, std::string>& headers,
      const std::string& json_body, int timeout_ms = 30000,
      CircuitBreaker* breaker = nullptr);
  fish::core::Result<std::string> download(const std::string& url,
                                           std::size_t max_bytes = 16u << 20);

 private:
  HttpClient() = default;
};

// ── Provider 配置 ───────────────────────────────────────
struct ChatConfig {
  std::string provider{"deepseek"};
  std::string base_url{"https://api.deepseek.com"};
  std::string api_key;
  std::string model{"deepseek-flash"};
  int max_tokens{512};
  double temperature{1.0};
  int timeout_ms{30000};

  // 上游是否支持「assistant 角色消息里带图片」。
  //
  // 这个值**不由配置文件决定**，而是由 fish.probe 自动探测后注入：
  //   - 启动时读缓存（默认 30 天有效），过期则重探
  //   - 上游行为会静默变更（如 WorkBuddy 会在 HTTP 200 下丢弃
  //     assistant 图片），靠人工配置必然过时
  // 见 fish.probe::Capabilities::assistant_image。
  bool supports_assistant_image{false};
};

struct EmbeddingConfig {
  std::string provider{"siliconflow"};
  std::string base_url{"https://api.siliconflow.cn/v1"};
  std::string api_key;
  std::string model{"BAAI/bge-m3"};
  int dimension{1024};
  int timeout_ms{20000};
};

struct SttConfig {
  std::string provider{"siliconflow"};
  std::string base_url{"https://api.siliconflow.cn/v1"};
  std::string api_key;
  std::string model{"FunAudioLLM/SenseVoiceSmall"};
  std::string language{"auto"};
  int timeout_ms{30000};
};

struct TtsConfig {
  std::string provider{"edge"};
  std::string base_url;
  std::string api_key;
  std::string voice{"zh-CN-XiaoyiNeural"};
  std::string format{"mp3"};
  int timeout_ms{20000};
};

// 网络搜索。与语音/生图同构：换厂商只改 provider/base_url。
//
// provider 取值：
//   none      —— 不启用（模型看不到 web_search 工具）
//   tavily    —— Tavily 官方
//   generic   —— 任意 HTTP 端点（自建服务、聚合 API、本地桥接层）
//   deepseek / bocha —— 保留的别名，都走 generic 的解析逻辑
struct SearchConfig {
  std::string provider{"none"};
  std::string base_url;
  std::string api_key;
  int max_results{5};
  int timeout_ms{15000};

  // ── DeepSeek 官方 Search / Fetch（provider = "deepseek"）──
  // 端点：{base_url}/v1/tools/search 和 /v1/tools/fetch
  // 计费：每 1000 次成功请求 5 元（失败/空结果不计费）
  // 限速：Search 100 QPS / Fetch 25 QPS（账号级）
  //
  // search：是否要求返回网页正文（need_content）
  bool search_need_content{false};
  // fetch：给模型看的正文上限（防止超长页面撑爆上下文）
  std::size_t fetch_max_chars{6000};
};

// 生图。与 TTS 同构：换厂商只改 provider/base_url/model。
struct ImageConfig {
  std::string provider{"none"};        // none | siliconflow | openai | bridge
  std::string base_url;
  std::string api_key;
  std::string model;
  std::string default_size{"1024x1024"};
  int timeout_ms{120000};              // 生图慢，给足时间
};

const char* chat_provider_list();
const char* embedding_provider_list();
const char* stt_provider_list();
const char* tts_provider_list();
const char* search_provider_list();
const char* image_provider_list();

fish::core::Result<std::unique_ptr<fish::core::IChatModel>> make_chat(const ChatConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::IEmbeddingModel>> make_embedding(
    const EmbeddingConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::ISpeechToText>> make_stt(const SttConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::ITextToSpeech>> make_tts(const TtsConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::IWebSearch>> make_search(
    const SearchConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::IWebFetch>> make_fetch(
    const SearchConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::IImageModel>> make_image(const ImageConfig& cfg);

ChatConfig load_chat_config(const fish::config::Config& c);
EmbeddingConfig load_embedding_config(const fish::config::Config& c);
SttConfig load_stt_config(const fish::config::Config& c);
TtsConfig load_tts_config(const fish::config::Config& c);
SearchConfig load_search_config(const fish::config::Config& c);
ImageConfig load_image_config(const fish::config::Config& c);

}  // namespace fish::capability

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::capability {

using fish::core::ErrorCode;
using fish::core::Result;
using json = nlohmann::json;

const char* chat_provider_list() {
  return "anthropic,deepseek,ollama,openai,siliconflow";
}
const char* embedding_provider_list() { return "ollama,openai,siliconflow"; }
const char* stt_provider_list() { return "groq,local,openai,siliconflow"; }
const char* tts_provider_list() { return "edge,openai,siliconflow"; }
const char* search_provider_list() { return "bocha,deepseek,none,tavily"; }
const char* image_provider_list() { return "bridge,openai,siliconflow"; }

namespace {

enum class Dialect { kOpenAI, kAnthropic };

Dialect dialect_of(const std::string& provider) {
  return provider == "anthropic" ? Dialect::kAnthropic : Dialect::kOpenAI;
}

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  const std::size_t total = size * nmemb;
  if (out->size() + total > (32u << 20)) return 0;  // 32MB 上限
  out->append(ptr, total);
  return total;
}

std::size_t header_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* map = static_cast<std::map<std::string, std::string>*>(userdata);
  const std::size_t total = size * nmemb;
  std::string line(ptr, total);
  const auto colon = line.find(':');
  if (colon != std::string::npos) {
    std::string k = line.substr(0, colon);
    std::string v = line.substr(colon + 1);
    auto trim = [](std::string& s) {
      while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
        s.pop_back();
      }
      std::size_t i = 0;
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
      if (i > 0) s.erase(0, i);
    };
    trim(k);
    trim(v);
    (*map)[k] = v;
  }
  return total;
}

bool is_retryable(int status) { return status == 429 || (status >= 500 && status < 600); }

// assistant 发图在「上游不支持」时的降级归属标签。
//
// 用结构化 XML 风格而非自然语言：
//   - 不会被模型误读成用户说的话
//   - 与正文明显区分，便于模型解析
//   - 换回支持 assistant 图的上游时，这段不会出现
constexpr const char* kAssistantImageTag =
    "<assistant_attachment role=\"assistant\" type=\"image\">"
    "这条消息的图片是 assistant 发送的，不是 user 发的。"
    "描述图片内容时，请明确这是 assistant 自己发过的图。"
    "</assistant_attachment>";

// 本地文件 → data URI（base64）。
// 模型只接受 http(s) URL 或 data URI，不认本地路径。
// 读不到或超过上限时返回空串，调用方跳过该块。
std::string local_file_to_data_uri(const std::string& path,
                                   const std::string& mime_hint = "") {
  constexpr std::size_t kMaxBytes = 8u << 20;   // 单图 8MB 上限

  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return {};
  const auto size = in.tellg();
  if (size <= 0 || static_cast<std::size_t>(size) > kMaxBytes) return {};
  in.seekg(0);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  in.read(bytes.data(), size);

  std::string mime = mime_hint;
  if (mime.empty() || mime == "image/png") {
    // 按扩展名纠正：缓存里存的是 .png/.jpg，但 mime 可能没带上
    const auto dot = path.find_last_of('.');
    const std::string ext =
        (dot == std::string::npos) ? "" : path.substr(dot);
    if (ext == ".jpg" || ext == ".jpeg") {
      mime = "image/jpeg";
    } else if (ext == ".png") {
      mime = "image/png";
    } else if (ext == ".gif") {
      mime = "image/gif";
    } else if (ext == ".webp") {
      mime = "image/webp";
    } else if (mime.empty()) {
      mime = "image/png";
    }
  }

  // 极简 base64（注意 unsigned char 转换，避免符号扩展）
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string b64;
  b64.reserve(((bytes.size() + 2) / 3) * 4);
  const auto at = [&bytes](std::size_t i) -> unsigned {
    return static_cast<unsigned>(static_cast<unsigned char>(bytes[i]));
  };
  std::size_t i = 0;
  while (i + 2 < bytes.size()) {
    const unsigned n = (at(i) << 16) | (at(i + 1) << 8) | at(i + 2);
    b64 += kTable[(n >> 18) & 0x3F];
    b64 += kTable[(n >> 12) & 0x3F];
    b64 += kTable[(n >> 6) & 0x3F];
    b64 += kTable[n & 0x3F];
    i += 3;
  }
  const std::size_t rest = bytes.size() - i;
  if (rest == 1) {
    const unsigned n = at(i) << 16;
    b64 += kTable[(n >> 18) & 0x3F];
    b64 += kTable[(n >> 12) & 0x3F];
    b64 += "==";
  } else if (rest == 2) {
    const unsigned n = (at(i) << 16) | (at(i + 1) << 8);
    b64 += kTable[(n >> 18) & 0x3F];
    b64 += kTable[(n >> 12) & 0x3F];
    b64 += kTable[(n >> 6) & 0x3F];
    b64 += '=';
  }
  return "data:" + mime + ";base64," + b64;
}

json to_blocks(const std::vector<fish::core::ContentBlock>& blocks) {
  json arr = json::array();
  for (const auto& b : blocks) {
    if (b.kind == fish::core::BlockKind::kText) {
      arr.push_back({{"type", "text"}, {"text", b.text}});
    } else if (b.kind == fish::core::BlockKind::kImage) {
      arr.push_back({{"type", "image_url"}, {"image_url", {{"url", b.media_url}}}});
    }
  }
  return arr;
}

void extract_usage(const json& usage, fish::core::IChatModel::Response* out) {
  if (!usage.is_object()) return;
  auto pick = [&](std::initializer_list<const char*> keys) -> int {
    for (const char* k : keys) {
      if (usage.contains(k) && usage[k].is_number()) return usage[k].get<int>();
    }
    return 0;
  };
  out->prompt_tokens = pick({"prompt_tokens", "input_tokens"});
  out->output_tokens = pick({"completion_tokens", "output_tokens"});

  int cached = pick({"prompt_cache_hit_tokens"});
  if (cached == 0 && usage.contains("prompt_tokens_details") &&
      usage["prompt_tokens_details"].is_object()) {
    const auto& d = usage["prompt_tokens_details"];
    if (d.contains("cached_tokens") && d["cached_tokens"].is_number()) {
      cached = d["cached_tokens"].get<int>();
    }
  }
  out->cached_tokens = cached;
}

// ── 文本生成 ────────────────────────────────────────────
class HttpChat final : public fish::core::IChatModel {
 public:
  HttpChat(ChatConfig cfg, Dialect d, CircuitBreaker* b)
      : cfg_(std::move(cfg)), dial_(d), breaker_(b) {}

  Result<Response> complete(const Request& req) override {
    json payload;
    payload["model"] = cfg_.model;
    payload["max_tokens"] = req.max_tokens > 0 ? req.max_tokens : cfg_.max_tokens;
    payload["temperature"] = req.temperature >= 0 ? req.temperature : cfg_.temperature;

    json msgs = json::array();
    if (dial_ == Dialect::kOpenAI) {
      // system 单独置前，保证前缀缓存命中
      msgs.push_back({{"role", "system"}, {"content", req.system_prompt}});
    } else {
      payload["system"] = req.system_prompt;
    }
    // 历史消息按「相邻同角色合并」的方式组装。
    //
    // 关于 assistant 的图片：
    //   上游支持（supports_assistant_image=true）时按正常语义走，
    //   assistant 消息里直接带 image_url。
    //   不支持时（如 WorkBuddy /v2/chat/completions 会静默丢弃），
    //   仅在该图片位置插入一条独立的 user 消息，内容只有归属标签 + 图。
    //   文字消息仍然走原本的 role，不做任何改变。
    std::string cur_role;
    json cur_parts = json::array();
    bool cur_has_parts = false;

    auto flush_history = [&]() {
      if (!cur_has_parts) return;
      // 只有一个 text 块时用纯字符串，兼容性最好
      if (cur_parts.size() == 1 && cur_parts[0].value("type", "") == "text") {
        msgs.push_back({{"role", cur_role}, {"content", cur_parts[0].value("text", "")}});
      } else {
        msgs.push_back({{"role", cur_role}, {"content", cur_parts}});
      }
      cur_parts = json::array();
      cur_has_parts = false;
      cur_role.clear();
    };

    // 本地路径 → data URI；失败返回空串
    auto to_url = [](const std::string& raw, const std::string& mime) -> std::string {
      if (raw.empty()) return {};
      if (raw.rfind("http://", 0) == 0 || raw.rfind("https://", 0) == 0 ||
          raw.rfind("data:", 0) == 0) {
        return raw;
      }
      return local_file_to_data_uri(raw, mime);
    };

    for (const auto& h : req.history) {
      const std::string role = (h.role == "assistant") ? "assistant" : "user";

      if (h.kind == fish::core::BlockKind::kImage) {
        const std::string url = to_url(h.media_url, h.mime);
        if (url.empty()) continue;   // 读不到就跳过，不要发死路径

        if (role == "assistant" && !cfg_.supports_assistant_image) {
          // 只在图片这一处做降级：先收尾当前消息，再插一条
          // 只含「归属标签 + 图」的 user 消息，然后清空合并状态，
          // 让后续文字重新按正常 role 走。
          flush_history();
          json parts = json::array();
          parts.push_back({{"type", "text"}, {"text", std::string(kAssistantImageTag)}});
          parts.push_back({{"type", "image_url"}, {"image_url", {{"url", url}}}});
          msgs.push_back({{"role", "user"}, {"content", parts}});
          continue;
        }

        if (role != cur_role) {
          flush_history();
          cur_role = role;
        }
        cur_parts.push_back({{"type", "image_url"}, {"image_url", {{"url", url}}}});
        cur_has_parts = true;
        continue;
      }

      if (h.kind == fish::core::BlockKind::kText) {
        if (h.text.empty()) continue;
        if (role != cur_role) {
          flush_history();
          cur_role = role;
        }
        cur_parts.push_back({{"type", "text"}, {"text", h.text}});
        cur_has_parts = true;
      }
    }
    flush_history();

    msgs.push_back({{"role", "user"}, {"content", to_blocks(req.user_blocks)}});
    payload["messages"] = std::move(msgs);

    // 工具声明：只在调用方提供了 tools 时加，避免无谓的 token 消耗
    if (!req.tools_json.empty()) {
      try {
        payload["tools"] = json::parse(req.tools_json);
        payload["tool_choice"] = "auto";
      } catch (const std::exception& e) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "capability", "tools_parse")
            .field("detail", e.what());
      }
    }

    std::map<std::string, std::string> headers;
    if (dial_ == Dialect::kOpenAI) {
      headers["Authorization"] = "Bearer " + cfg_.api_key;
    } else {
      headers["x-api-key"] = cfg_.api_key;
      headers["anthropic-version"] = "2023-06-01";
    }

    const std::string url =
        cfg_.base_url + (dial_ == Dialect::kOpenAI ? "/chat/completions" : "/v1/messages");

    auto resp =
        HttpClient::instance().post_json(url, headers, payload.dump(), cfg_.timeout_ms, breaker_);
    if (!resp.ok()) return Result<Response>::fail(resp.error.code, resp.error.detail);

    Response out;
    try {
      const auto body = json::parse(resp.value->body);
      if (dial_ == Dialect::kOpenAI) {
        if (!body.contains("choices") || body["choices"].empty()) {
          return Result<Response>::fail(ErrorCode::kBadRequest, "no_choices");
        }
        out.text = body["choices"][0]["message"].value("content", "");
        extract_usage(body.value("usage", json::object()), &out);

        // 解析工具调用：上游可能返回 tool_calls 数组
        const auto msg = body["choices"][0].value("message", json::object());
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
          for (const auto& tc : msg["tool_calls"]) {
            if (!tc.is_object()) continue;
            const auto fn = tc.value("function", json::object());
            IChatModel::ToolCall call;
            call.id = tc.value("id", "");
            call.name = fn.value("name", "");
            // arguments 可能是字符串，也可能是对象
            if (fn.contains("arguments")) {
              if (fn["arguments"].is_string()) {
                call.arguments = fn["arguments"].get<std::string>();
              } else {
                call.arguments = fn["arguments"].dump();
              }
            }
            if (!call.name.empty()) out.tool_calls.push_back(std::move(call));
          }
          if (!out.tool_calls.empty()) {
            ::fish::log::log_line(::fish::log::LogLevel::kInfo, "capability", "tool_calls")
                .field("count", static_cast<long long>(out.tool_calls.size()))
                .field("first", out.tool_calls[0].name);
          }
        }
      } else {
        if (!body.contains("content") || !body["content"].is_array()) {
          return Result<Response>::fail(ErrorCode::kBadRequest, "no_content");
        }
        for (const auto& blk : body["content"]) {
          if (blk.value("type", "") == "text") out.text += blk.value("text", "");
        }
        extract_usage(body.value("usage", json::object()), &out);
      }
    } catch (const std::exception& e) {
      return Result<Response>::fail(ErrorCode::kBadRequest,
                                    std::string("parse:") + e.what());
    }

    if (out.text.empty()) {
      return Result<Response>::fail(ErrorCode::kUpstreamUnavailable, "empty_completion");
    }

    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "capability", "chat_ok")
        .field("provider", cfg_.provider)
        .field("prompt", out.prompt_tokens)
        .field("cached", out.cached_tokens)
        .field("output", out.output_tokens);

    return Result<Response>::ok(std::move(out));
  }

 private:
  ChatConfig cfg_;
  Dialect dial_;
  CircuitBreaker* breaker_;
};

// ── 向量 ────────────────────────────────────────────────
class HttpEmbedding final : public fish::core::IEmbeddingModel {
 public:
  HttpEmbedding(EmbeddingConfig cfg, CircuitBreaker* b)
      : cfg_(std::move(cfg)), breaker_(b) {}

  int dimension() const noexcept override { return cfg_.dimension; }

  Result<std::vector<float>> embed(const std::string& text) override {
    json payload{{"model", cfg_.model}, {"input", text}};
    std::map<std::string, std::string> headers{{"Authorization", "Bearer " + cfg_.api_key}};

    auto resp = HttpClient::instance().post_json(cfg_.base_url + "/embeddings", headers,
                                                 payload.dump(), cfg_.timeout_ms, breaker_);
    if (!resp.ok()) return Result<std::vector<float>>::fail(resp.error.code, resp.error.detail);

    try {
      const auto body = json::parse(resp.value->body);
      if (!body.contains("data") || body["data"].empty()) {
        return Result<std::vector<float>>::fail(ErrorCode::kBadRequest, "no_data");
      }
      std::vector<float> out;
      for (const auto& v : body["data"][0]["embedding"]) out.push_back(v.get<float>());
      return Result<std::vector<float>>::ok(std::move(out));
    } catch (const std::exception& e) {
      return Result<std::vector<float>>::fail(ErrorCode::kBadRequest,
                                              std::string("parse:") + e.what());
    }
  }

 private:
  EmbeddingConfig cfg_;
  CircuitBreaker* breaker_;
};

// ── 语音识别 ────────────────────────────────────────────
class HttpStt final : public fish::core::ISpeechToText {
 public:
  HttpStt(SttConfig cfg, CircuitBreaker* b) : cfg_(std::move(cfg)), breaker_(b) {}

  Result<std::string> transcribe(const std::string& media_url) override {
    auto audio = HttpClient::instance().download(media_url, 16u << 20);
    if (!audio.ok()) return Result<std::string>::fail(audio.error.code, audio.error.detail);

    // 扩展名必须与真实格式一致：SiliconFlow 按扩展名判断解码器，
    // 存成 .audio 会 400。按文件头嗅探，避免依赖 URL 后缀。
    const std::string ext = sniff_audio_ext(audio.value->data());
    std::string tmp = "/tmp/fish_stt_" + std::to_string(std::rand()) + ext;
    {
      std::ofstream f(tmp, std::ios::binary);
      if (!f) return Result<std::string>::fail(ErrorCode::kInternal, "tmp_write");
      f.write(audio.value->data(), static_cast<std::streamsize>(audio.value->size()));
    }

    // SILK 是腾讯自有格式，STT 服务不支持，必须先转 WAV。
    // 用 tools/silk2wav.py（内部优先 ffmpeg，否则用纯 Python 的 pilk）。
    if (ext == ".silk") {
      const std::string wav = "/tmp/fish_stt_" + std::to_string(std::rand()) + ".wav";
      const std::string cmd = "python3 tools/silk2wav.py '" + tmp + "' '" + wav +
                              "' --rate 16000 >/dev/null 2>&1";
      const int rc = std::system(cmd.c_str());
      std::remove(tmp.c_str());
      if (rc != 0) {
        return Result<std::string>::fail(ErrorCode::kUnsupported,
                                         "silk_convert_failed（装 ffmpeg 或 pilk）");
      }
      tmp = wav;
    }

    const std::string cmd = "curl -s -X POST '" + cfg_.base_url + "/audio/transcriptions' "
                            "-H 'Authorization: Bearer " + cfg_.api_key + "' "
                            "-F 'file=@" + tmp + "' -F 'model=" + cfg_.model + "' "
                            "-F 'language=" + cfg_.language + "' 2>/dev/null";
    std::string out;
    if (FILE* pipe = popen(cmd.c_str(), "r")) {
      char buf[4096];
      while (std::fgets(buf, sizeof(buf), pipe) != nullptr) out += buf;
      pclose(pipe);
    }
    std::remove(tmp.c_str());

    if (out.empty()) {
      return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "stt_empty");
    }
    try {
      const auto body = json::parse(out);
      if (body.contains("text")) return Result<std::string>::ok(body["text"].get<std::string>());
      return Result<std::string>::fail(ErrorCode::kBadRequest, "stt_no_text");
    } catch (const std::exception& e) {
      return Result<std::string>::fail(ErrorCode::kBadRequest,
                                       std::string("stt_parse:") + e.what());
    }
  }

 private:
  // 按文件头判断音频格式，返回带点的扩展名。
  // QQ 的语音常见 SILK（腾讯自有），也有 mp3/wav/ogg。
  static std::string sniff_audio_ext(const std::string& data) {
    if (data.size() < 12) return ".bin";
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    // SILK: 以 "#!SILK_V3" 或 0x02 "#!SILK" 开头
    if (data.rfind("#!SILK", 0) == 0 || (p[0] == 0x02 && data.rfind("#!SILK", 1) == 0)) {
      return ".silk";
    }
    // ID3 或 0xFF 0xFB/0xF3/0xF2 → mp3
    if (data.rfind("ID3", 0) == 0 ||
        (p[0] == 0xFF && (p[1] & 0xE0) == 0xE0)) {
      return ".mp3";
    }
    // RIFF....WAVE
    if (data.size() > 12 && data.rfind("RIFF", 0) == 0 && data.substr(8, 4) == "WAVE") {
      return ".wav";
    }
    // OggS
    if (data.rfind("OggS", 0) == 0) return ".ogg";
    // fLaC
    if (data.rfind("fLaC", 0) == 0) return ".flac";
    // AMR
    if (data.rfind("#!AMR", 0) == 0) return ".amr";
    return ".bin";
  }

  SttConfig cfg_;
  CircuitBreaker* breaker_;
};

// ── 语音合成 ────────────────────────────────────────────
class EdgeTts final : public fish::core::ITextToSpeech {
 public:
  explicit EdgeTts(TtsConfig cfg) : cfg_(std::move(cfg)) {}

  Result<std::string> synthesize(const std::string& text) override {
    const std::string tmp = "/tmp/fish_tts_" + std::to_string(std::rand()) + "." + cfg_.format;
    std::string safe;
    for (const char c : text) {
      if (c == '\'') {
        safe += "'\\''";
      } else if (c != '\n' && c != '\r') {
        safe += c;
      }
    }
    // edge-tts 常被 pip 装到 ~/.local/bin，不在默认 PATH 里，
    // 所以优先用绝对路径，找不到再退回裸命令名。
    const std::string exe = resolve_edge_tts();
    if (exe.empty()) {
      return Result<std::string>::fail(
          ErrorCode::kUnsupported,
          "edge_tts_not_found（pip install edge-tts）");
    }
    const std::string cmd = "'" + exe + "' --voice '" + cfg_.voice + "' --text '" + safe +
                            "' --write-media '" + tmp + "' 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) {
      return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "edge_tts_failed");
    }
    std::ifstream f(tmp, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() <= 0) {
      std::remove(tmp.c_str());
      return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "edge_tts_empty");
    }
    return Result<std::string>::ok(tmp);
  }

 private:
  // 依次尝试：绝对路径 → PATH 里的 edge-tts
  static std::string resolve_edge_tts() {
    static const char* kCandidates[] = {
        "/usr/local/bin/edge-tts",
        "/usr/bin/edge-tts",
    };
    for (const char* p : kCandidates) {
      std::ifstream f(p);
      if (f.good()) return p;
    }
    // ~/.local/bin（pip --user 的默认位置）
    if (const char* home = std::getenv("HOME")) {
      const std::string p = std::string(home) + "/.local/bin/edge-tts";
      std::ifstream f(p);
      if (f.good()) return p;
    }
    // 退回裸命令名，让 shell 去 PATH 里找
    if (std::system("command -v edge-tts >/dev/null 2>&1") == 0) return "edge-tts";
    return {};
  }

  TtsConfig cfg_;
};

class HttpTts final : public fish::core::ITextToSpeech {
 public:
  HttpTts(TtsConfig cfg, CircuitBreaker* b) : cfg_(std::move(cfg)), breaker_(b) {}

  Result<std::string> synthesize(const std::string& text) override {
    json payload{{"model", "tts-1"}, {"input", text}, {"voice", cfg_.voice},
                 {"response_format", cfg_.format}};
    HttpRequest req;
    req.url = cfg_.base_url + "/audio/speech";
    req.headers = {{"Authorization", "Bearer " + cfg_.api_key},
                   {"Content-Type", "application/json"}};
    req.body = payload.dump();
    req.timeout_ms = cfg_.timeout_ms;
    req.max_retries = 1;
    req.want_binary = true;

    auto resp = HttpClient::instance().perform(req, breaker_);
    if (!resp.ok()) return Result<std::string>::fail(resp.error.code, resp.error.detail);
    if (resp.value->bytes.empty()) {
      return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "tts_empty");
    }

    const std::string tmp = "/tmp/fish_tts_" + std::to_string(std::rand()) + "." + cfg_.format;
    std::ofstream f(tmp, std::ios::binary);
    if (!f) return Result<std::string>::fail(ErrorCode::kInternal, "tts_write");
    f.write(resp.value->bytes.data(), static_cast<std::streamsize>(resp.value->bytes.size()));
    return Result<std::string>::ok(tmp);
  }

 private:
  TtsConfig cfg_;
  CircuitBreaker* breaker_;
};

// ── 生图 ────────────────────────────────────────────────
// 走 OpenAI 兼容的 /images/generations（文生图）。
// 返回可能是 b64_json 或 url，两种都处理：
//   - b64_json → 落盘到 /tmp，返回本地路径（通道可直接上传）
//   - url      → 直接返回 URL
class HttpImage final : public fish::core::IImageModel {
 public:
  HttpImage(ImageConfig cfg, CircuitBreaker* b) : cfg_(std::move(cfg)), breaker_(b) {}

  Result<Response> generate(const Request& in) override {
    if (cfg_.provider == "none") {
      return Result<Response>::fail(ErrorCode::kUnsupported, "image_provider_none");
    }

    json payload{{"model", cfg_.model}, {"prompt", in.prompt}, {"n", 1}};
    const std::string size = in.size.empty() ? cfg_.default_size : in.size;
    if (!size.empty()) payload["size"] = size;
    if (!in.negative_prompt.empty()) payload["negative_prompt"] = in.negative_prompt;
    // 只要 base64，省一次额外下载；不支持的厂商会忽略该字段
    payload["response_format"] = "b64_json";

    HttpRequest req;
    req.url = cfg_.base_url + "/images/generations";
    req.headers = {{"Authorization", "Bearer " + cfg_.api_key},
                   {"Content-Type", "application/json"}};
    req.body = payload.dump();
    req.timeout_ms = cfg_.timeout_ms;
    req.max_retries = 1;   // 生图失败重试代价高，不自动重试

    auto resp = HttpClient::instance().perform(req, breaker_);
    if (!resp.ok()) return Result<Response>::fail(resp.error.code, resp.error.detail);

    Response out;
    out.model = cfg_.model;
    out.actual_size = size;

    try {
      const auto body = json::parse(resp.value->body);
      if (!body.contains("data") || !body["data"].is_array() || body["data"].empty()) {
        return Result<Response>::fail(ErrorCode::kBadRequest, "image_no_data");
      }
      const auto& item = body["data"][0];

      // 优先 b64_json
      if (item.contains("b64_json") && item["b64_json"].is_string()) {
        const std::string b64 = item["b64_json"].get<std::string>();
        auto bin = base64_decode(b64);
        if (bin.empty()) {
          return Result<Response>::fail(ErrorCode::kBadRequest, "image_b64_bad");
        }
        const std::string path =
            "/tmp/fish_img_" + std::to_string(std::rand()) + ".png";
        std::ofstream f(path, std::ios::binary);
        if (!f) return Result<Response>::fail(ErrorCode::kInternal, "image_write");
        f.write(reinterpret_cast<const char*>(bin.data()),
                static_cast<std::streamsize>(bin.size()));
        out.image_url = path;
        out.usage_note = "b64 " + std::to_string(bin.size()) + "B";
        return Result<Response>::ok(std::move(out));
      }

      // 退而求其次：url
      if (item.contains("url") && item["url"].is_string()) {
        out.image_url = item["url"].get<std::string>();
        out.usage_note = "url";
        return Result<Response>::ok(std::move(out));
      }

      return Result<Response>::fail(ErrorCode::kBadRequest, "image_no_url");
    } catch (const std::exception& e) {
      return Result<Response>::fail(ErrorCode::kBadRequest,
                                    std::string("image_parse:") + e.what());
    }
  }

 private:
  // 极简 base64 解码（避免引入额外依赖）
  static std::vector<unsigned char> base64_decode(const std::string& s) {
    static const auto table = [] {
      std::array<int, 256> t{};
      t.fill(-1);
      const char* chars =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(chars[i])] = i;
      return t;
    }();

    std::vector<unsigned char> out;
    out.reserve(s.size() * 3 / 4);
    int val = 0;
    int bits = -8;
    for (const char c : s) {
      if (c == '=' || c == '\n' || c == '\r') continue;
      const int d = table[static_cast<unsigned char>(c)];
      if (d < 0) continue;
      val = (val << 6) + d;
      bits += 6;
      if (bits >= 0) {
        out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
        bits -= 8;
      }
    }
    return out;
  }

  ImageConfig cfg_;
  CircuitBreaker* breaker_;
};

// ── 搜索 ────────────────────────────────────────────────
class NullSearch final : public fish::core::IWebSearch {
 public:
  Result<std::vector<Hit>> search(const std::string&, int) override {
    return Result<std::vector<Hit>>::ok({});
  }
};

class TavilySearch final : public fish::core::IWebSearch {
 public:
  explicit TavilySearch(SearchConfig cfg) : cfg_(std::move(cfg)) {}

  Result<std::vector<Hit>> search(const std::string& query, int max_results) override {
    json payload{{"api_key", cfg_.api_key}, {"query", query}, {"max_results", max_results}};
    const std::string url =
        cfg_.base_url.empty() ? "https://api.tavily.com/search" : cfg_.base_url + "/search";
    auto resp = HttpClient::instance().post_json(url, {}, payload.dump(), cfg_.timeout_ms);
    if (!resp.ok()) return Result<std::vector<Hit>>::fail(resp.error.code, resp.error.detail);

    std::vector<Hit> hits;
    try {
      const auto body = json::parse(resp.value->body);
      for (const auto& r : body.value("results", json::array())) {
        Hit h;
        h.title = r.value("title", "");
        h.url = r.value("url", "");
        h.snippet = r.value("content", "");
        h.score = r.value("score", 0.0);
        hits.push_back(std::move(h));
      }
    } catch (const std::exception& e) {
      return Result<std::vector<Hit>>::fail(ErrorCode::kBadRequest,
                                            std::string("parse:") + e.what());
    }
    return Result<std::vector<Hit>>::ok(std::move(hits));
  }

 private:
  SearchConfig cfg_;
};

// DeepSeek 官方 Search API。
// 端点：POST {base_url}/v1/tools/search
// 文档：https://api-docs.deepseek.com/zh-cn/guides/search/
//
// 特点：
//   - 原始 JSON API，不消耗模型轮次
//   - 每 1000 次成功请求 5 元（失败/空结果不计费）
//   - 返回结构化结果：title/url/snippet/summary/content/date/authority
//   - 支持搜索语法：site: / -site: / -词 / "精确短语" / lang: / after: / before:
class DeepSeekSearch final : public fish::core::IWebSearch {
 public:
  explicit DeepSeekSearch(SearchConfig cfg) : cfg_(std::move(cfg)) {}

  Result<std::vector<Hit>> search(const std::string& query, int max_results) override {
    // query 限制 1~128 字符，不能全空白
    std::string q = query;
    if (q.size() > 128) q.resize(128);
    if (q.find_first_not_of(" \t\r\n") == std::string::npos) {
      return Result<std::vector<Hit>>::fail(ErrorCode::kBadRequest, "search_empty_query");
    }

    int count = max_results > 0 ? max_results : cfg_.max_results;
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    json payload{
        {"query", q},
        {"count", count},
        {"need_content", cfg_.search_need_content},
    };

    const std::string url = search_endpoint() + "/search";
    std::map<std::string, std::string> headers{
        {"Authorization", "Bearer " + cfg_.api_key},
    };

    auto resp = HttpClient::instance().post_json(url, headers, payload.dump(),
                                                 cfg_.timeout_ms);
    if (!resp.ok()) {
      // 402 余额不足 / 429 限流 / 5xx 可重试，已在 HttpClient 内处理退避
      return Result<std::vector<Hit>>::fail(resp.error.code,
                                            "search_" + resp.error.detail);
    }

    try {
      const auto body = json::parse(resp.value->body);
      std::vector<Hit> hits;
      for (const auto& r : body.value("results", json::array())) {
        if (!r.is_object()) continue;
        Hit h;
        h.title = r.value("title", "");
        h.url = r.value("url", "");
        // summary 是给模型读的长摘要（600-800 字符），snippet 是给前端看的短摘要
        // 优先用 summary，模型理解更完整
        h.snippet = r.value("summary", "");
        if (h.snippet.empty()) h.snippet = r.value("snippet", "");
        if (r.contains("rerank_score") && r["rerank_score"].is_number()) {
          h.score = r["rerank_score"].get<double>();
        }
        if (!h.title.empty() || !h.url.empty()) hits.push_back(std::move(h));
      }
      return Result<std::vector<Hit>>::ok(std::move(hits));
    } catch (const std::exception& e) {
      return Result<std::vector<Hit>>::fail(ErrorCode::kBadRequest,
                                            std::string("search_parse:") + e.what());
    }
  }

 private:
  // base_url 可能带或不带 /v1，统一归一化
  std::string search_endpoint() const {
    std::string base = cfg_.base_url.empty() ? "https://api.deepseek.com" : cfg_.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.find("/v1/tools") != std::string::npos) return base;
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0) {
      return base + "/tools";
    }
    return base + "/v1/tools";
  }

  SearchConfig cfg_;
};

// DeepSeek 官方 Fetch API。
// 端点：POST {base_url}/v1/tools/fetch
// 返回 {title, content}，content 是 Markdown 正文。
//
// 注意：fetch 由 DeepSeek 服务端执行，不存在 SSRF 风险
// （我们不发请求到目标站点，只发给 DeepSeek）。
// 403 表示被安全策略拒绝，404 表示抓不到可用内容。
class DeepSeekFetch final : public fish::core::IWebFetch {
 public:
  explicit DeepSeekFetch(SearchConfig cfg) : cfg_(std::move(cfg)) {}

  Result<Page> fetch(const std::string& url) override {
    // 基本校验：必须是含 host 的 HTTP/HTTPS URL，不能带用户名密码
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
      return Result<Page>::fail(ErrorCode::kBadRequest, "fetch_scheme_not_allowed");
    }
    if (url.size() > 8192) {
      return Result<Page>::fail(ErrorCode::kBadRequest, "fetch_url_too_long");
    }
    const auto scheme_end = url.find("://");
    const auto host_start = scheme_end + 3;
    if (host_start >= url.size() || url[host_start] == '/') {
      return Result<Page>::fail(ErrorCode::kBadRequest, "fetch_missing_host");
    }
    // 拒绝带 userinfo 的 URL（可能用于钓鱼/凭据泄露）
    const auto authority_end = url.find_first_of("/?#", host_start);
    const std::string authority =
        url.substr(host_start, authority_end == std::string::npos
                                   ? std::string::npos
                                   : authority_end - host_start);
    if (authority.find('@') != std::string::npos) {
      return Result<Page>::fail(ErrorCode::kBadRequest, "fetch_userinfo_not_allowed");
    }

    json payload{{"url", url}};
    const std::string endpoint = fetch_endpoint() + "/fetch";
    std::map<std::string, std::string> headers{
        {"Authorization", "Bearer " + cfg_.api_key},
    };

    auto resp = HttpClient::instance().post_json(endpoint, headers, payload.dump(),
                                                 cfg_.timeout_ms);
    if (!resp.ok()) {
      return Result<Page>::fail(resp.error.code, "fetch_" + resp.error.detail);
    }

    try {
      const auto body = json::parse(resp.value->body);
      Page page;
      page.url = url;
      page.title = body.value("title", "");
      page.content = body.value("content", "");
      page.content_type = "text/markdown";
      page.status = 200;
      page.bytes = page.content.size();

      if (page.content.empty()) {
        return Result<Page>::fail(ErrorCode::kUpstreamUnavailable, "fetch_empty_content");
      }

      // 截断：超长页面会撑爆上下文，按配置截到合理长度
      if (page.content.size() > cfg_.fetch_max_chars) {
        std::size_t cut = cfg_.fetch_max_chars;
        while (cut > 0 &&
               (static_cast<unsigned char>(page.content[cut]) & 0xC0) == 0x80) {
          --cut;
        }
        page.content.resize(cut);
        page.content += "\n\n…（内容过长已截断）";
      }
      return Result<Page>::ok(std::move(page));
    } catch (const std::exception& e) {
      return Result<Page>::fail(ErrorCode::kBadRequest,
                                std::string("fetch_parse:") + e.what());
    }
  }

 private:
  std::string fetch_endpoint() const {
    std::string base = cfg_.base_url.empty() ? "https://api.deepseek.com" : cfg_.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.find("/v1/tools") != std::string::npos) return base;
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0) {
      return base + "/tools";
    }
    return base + "/v1/tools";
  }

  SearchConfig cfg_;
};

CircuitBreaker& breaker_for(const std::string& provider, const char* kind) {
  static std::mutex mu;
  static std::map<std::string, std::unique_ptr<CircuitBreaker>> pool;
  std::lock_guard<std::mutex> lock(mu);
  const std::string key = provider + "/" + kind;
  auto it = pool.find(key);
  if (it == pool.end()) it = pool.emplace(key, std::make_unique<CircuitBreaker>(key)).first;
  return *it->second;
}

}  // namespace

// ── CircuitBreaker ──────────────────────────────────────
CircuitBreaker::CircuitBreaker(std::string name, int threshold, std::chrono::seconds cooldown)
    : name_(std::move(name)), threshold_(threshold), cooldown_(cooldown) {}

bool CircuitBreaker::is_open() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (failures_ < threshold_) return false;
  return fish::core::Clock::now() < open_until_;
}

bool CircuitBreaker::allow() const { return !is_open(); }

void CircuitBreaker::on_success() {
  std::lock_guard<std::mutex> lock(mu_);
  failures_ = 0;
}

void CircuitBreaker::on_failure() {
  std::lock_guard<std::mutex> lock(mu_);
  ++failures_;
  if (failures_ >= threshold_) {
    open_until_ = fish::core::Clock::now() + cooldown_;
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "http", "circuit_open")
        .field("provider", name_)
        .field("failures", failures_);
  }
}

// ── HttpClient ──────────────────────────────────────────
HttpClient& HttpClient::instance() {
  static HttpClient c;
  return c;
}

void HttpClient::global_init() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void HttpClient::global_cleanup() { curl_global_cleanup(); }

Result<HttpResult> HttpClient::perform(const HttpRequest& req, CircuitBreaker* breaker) {
  if (breaker != nullptr && !breaker->allow()) {
    return Result<HttpResult>::fail(ErrorCode::kUpstreamUnavailable, "circuit_open");
  }

  for (int attempt = 0; attempt <= req.max_retries; ++attempt) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      return Result<HttpResult>::fail(ErrorCode::kInternal, "curl_init");
    }

    HttpResult result;
    curl_slist* headers = nullptr;
    for (const auto& [k, v] : req.headers) {
      headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }

    std::string body_out;
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_out);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result.headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

    if (req.method == "POST") {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    } else if (req.method == "GET") {
      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    }

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    result.status = static_cast<int>(status);
    if (req.want_binary) {
      result.bytes = std::move(body_out);
    } else {
      result.body = std::move(body_out);
    }

    if (code == CURLE_OK && result.status >= 200 && result.status < 300) {
      if (breaker != nullptr) breaker->on_success();
      return Result<HttpResult>::ok(std::move(result));
    }

    const bool transport_err = code != CURLE_OK;
    const bool retryable = transport_err || is_retryable(result.status);
    if (breaker != nullptr) breaker->on_failure();

    if (!retryable || attempt == req.max_retries) {
      ErrorCode ec = ErrorCode::kUpstreamUnavailable;
      if (result.status == 429) {
        ec = ErrorCode::kRateLimited;
      } else if (result.status == 401 || result.status == 403) {
        ec = ErrorCode::kUnauthorized;
      } else if (result.status >= 400 && result.status < 500) {
        ec = ErrorCode::kBadRequest;
      }
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "http", "request_failed")
          .field("url", req.url)
          .field("status", result.status)
          // 4xx 多半是请求体不对，必须把上游说的话打出来，否则只能靠猜
          .field("body", result.body.substr(0, 400));
      return Result<HttpResult>::fail(ec, "http_error");
    }

    int wait_ms = 500 * (attempt + 1) * (attempt + 1);
    if (result.status == 429) {
      auto it = result.headers.find("retry-after");
      if (it != result.headers.end()) {
        try {
          wait_ms = std::stoi(it->second) * 1000;
        } catch (...) {
        }
      }
      wait_ms = std::max(wait_ms, 2000);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
  }
  return Result<HttpResult>::fail(ErrorCode::kUpstreamUnavailable, "retries_exhausted");
}

Result<HttpResult> HttpClient::post_json(const std::string& url,
                                         const std::map<std::string, std::string>& headers,
                                         const std::string& json_body, int timeout_ms,
                                         CircuitBreaker* breaker) {
  HttpRequest req;
  req.url = url;
  req.headers = headers;
  req.headers["Content-Type"] = "application/json";
  req.body = json_body;
  req.timeout_ms = timeout_ms;
  return perform(req, breaker);
}

Result<std::string> HttpClient::download(const std::string& url, std::size_t max_bytes) {
  HttpRequest req;
  req.method = "GET";
  req.url = url;
  req.want_binary = true;
  req.max_retries = 1;
  auto r = perform(req);
  if (!r.ok()) return Result<std::string>::fail(r.error.code, r.error.detail);
  if (r.value->bytes.size() > max_bytes) {
    return Result<std::string>::fail(ErrorCode::kBadRequest, "too_large");
  }
  return Result<std::string>::ok(std::move(r.value->bytes));
}

// ── 工厂 ────────────────────────────────────────────────
Result<std::unique_ptr<fish::core::IChatModel>> make_chat(const ChatConfig& cfg) {
  static const std::set<std::string> kOk{"deepseek", "openai", "anthropic", "siliconflow",
                                         "ollama"};
  if (!kOk.count(cfg.provider)) {
    return Result<std::unique_ptr<fish::core::IChatModel>>::fail(
        ErrorCode::kUnsupported,
        "unknown chat provider '" + cfg.provider + "'. supported: " + chat_provider_list());
  }
  if (cfg.api_key.empty() && cfg.provider != "ollama") {
    return Result<std::unique_ptr<fish::core::IChatModel>>::fail(ErrorCode::kUnauthorized,
                                                                 "chat api_key empty");
  }
  return Result<std::unique_ptr<fish::core::IChatModel>>::ok(std::make_unique<HttpChat>(
      cfg, dialect_of(cfg.provider), &breaker_for(cfg.provider, "chat")));
}

Result<std::unique_ptr<fish::core::IEmbeddingModel>> make_embedding(const EmbeddingConfig& cfg) {
  static const std::set<std::string> kOk{"siliconflow", "openai", "ollama"};
  if (!kOk.count(cfg.provider)) {
    return Result<std::unique_ptr<fish::core::IEmbeddingModel>>::fail(
        ErrorCode::kUnsupported, "unknown embedding provider '" + cfg.provider +
                                     "'. supported: " + embedding_provider_list());
  }
  return Result<std::unique_ptr<fish::core::IEmbeddingModel>>::ok(
      std::make_unique<HttpEmbedding>(cfg, &breaker_for(cfg.provider, "embedding")));
}

Result<std::unique_ptr<fish::core::ISpeechToText>> make_stt(const SttConfig& cfg) {
  static const std::set<std::string> kOk{"siliconflow", "groq", "openai", "local"};
  if (!kOk.count(cfg.provider)) {
    return Result<std::unique_ptr<fish::core::ISpeechToText>>::fail(
        ErrorCode::kUnsupported, "unknown stt provider '" + cfg.provider +
                                     "'. supported: " + stt_provider_list());
  }
  return Result<std::unique_ptr<fish::core::ISpeechToText>>::ok(
      std::make_unique<HttpStt>(cfg, &breaker_for(cfg.provider, "stt")));
}

Result<std::unique_ptr<fish::core::ITextToSpeech>> make_tts(const TtsConfig& cfg) {
  static const std::set<std::string> kOk{"edge", "openai", "siliconflow"};
  if (!kOk.count(cfg.provider)) {
    return Result<std::unique_ptr<fish::core::ITextToSpeech>>::fail(
        ErrorCode::kUnsupported, "unknown tts provider '" + cfg.provider +
                                     "'. supported: " + tts_provider_list());
  }
  if (cfg.provider == "edge") {
    return Result<std::unique_ptr<fish::core::ITextToSpeech>>::ok(std::make_unique<EdgeTts>(cfg));
  }
  return Result<std::unique_ptr<fish::core::ITextToSpeech>>::ok(
      std::make_unique<HttpTts>(cfg, &breaker_for(cfg.provider, "tts")));
}

Result<std::unique_ptr<fish::core::IWebSearch>> make_search(const SearchConfig& cfg) {
  static const std::set<std::string> kOk{"none", "deepseek", "tavily", "bocha"};
  if (!kOk.count(cfg.provider)) {
    return Result<std::unique_ptr<fish::core::IWebSearch>>::fail(
        ErrorCode::kUnsupported, "unknown search provider '" + cfg.provider +
                                     "'. supported: " + search_provider_list());
  }
  if (cfg.provider == "none") {
    return Result<std::unique_ptr<fish::core::IWebSearch>>::ok(std::make_unique<NullSearch>());
  }
  if (cfg.provider == "tavily") {
    return Result<std::unique_ptr<fish::core::IWebSearch>>::ok(
        std::make_unique<TavilySearch>(cfg));
  }
  // deepseek / bocha 走 DeepSeek 官方 Tools API 的实现
  return Result<std::unique_ptr<fish::core::IWebSearch>>::ok(
      std::make_unique<DeepSeekSearch>(cfg));
}

Result<std::unique_ptr<fish::core::IWebFetch>> make_fetch(const SearchConfig& cfg) {
  if (cfg.provider == "none") {
    return Result<std::unique_ptr<fish::core::IWebFetch>>::fail(
        ErrorCode::kUnsupported, "fetch_provider_none");
  }
  return Result<std::unique_ptr<fish::core::IWebFetch>>::ok(
      std::make_unique<DeepSeekFetch>(cfg));
}

Result<std::unique_ptr<fish::core::IImageModel>> make_image(const ImageConfig& cfg) {
  static const std::set<std::string> kOk{"none", "siliconflow", "openai", "bridge"};
  if (!kOk.count(cfg.provider)) {
    return Result<std::unique_ptr<fish::core::IImageModel>>::fail(
        ErrorCode::kUnsupported, "unknown image provider '" + cfg.provider +
                                     "'. supported: " + image_provider_list());
  }
  if (cfg.provider == "none") {
    return Result<std::unique_ptr<fish::core::IImageModel>>::fail(
        ErrorCode::kUnsupported, "image_provider_none");
  }
  return Result<std::unique_ptr<fish::core::IImageModel>>::ok(
      std::make_unique<HttpImage>(cfg, &breaker_for(cfg.provider, "image")));
}

// ── 配置读取 ────────────────────────────────────────────
ChatConfig load_chat_config(const fish::config::Config& c) {
  ChatConfig cfg;
  cfg.provider = c.get_or("capability.chat.provider", "deepseek");
  cfg.base_url = c.get_or("capability.chat.base_url", "https://api.deepseek.com");
  cfg.api_key = c.get_or("capability.chat.api_key", "");
  cfg.model = c.get_or("capability.chat.model", "deepseek-flash");
  cfg.max_tokens = static_cast<int>(c.get_int_or("capability.chat.max_tokens", 512));
  cfg.temperature = c.get_double_or("capability.chat.temperature", 1.0);
  cfg.timeout_ms = static_cast<int>(c.get_int_or("capability.chat.timeout_ms", 30000));
  // supports_assistant_image 不在这里读配置：
  // 由 fish.probe 探测后由 main 注入，避免配置过时。
  return cfg;
}

EmbeddingConfig load_embedding_config(const fish::config::Config& c) {
  EmbeddingConfig cfg;
  cfg.provider = c.get_or("capability.embedding.provider", "siliconflow");
  cfg.base_url = c.get_or("capability.embedding.base_url", "https://api.siliconflow.cn/v1");
  cfg.api_key = c.get_or("capability.embedding.api_key", "");
  cfg.model = c.get_or("capability.embedding.model", "BAAI/bge-m3");
  cfg.dimension = static_cast<int>(c.get_int_or("capability.embedding.dimension", 1024));
  return cfg;
}

SttConfig load_stt_config(const fish::config::Config& c) {
  SttConfig cfg;
  cfg.provider = c.get_or("capability.stt.provider", "siliconflow");
  cfg.base_url = c.get_or("capability.stt.base_url", "https://api.siliconflow.cn/v1");
  cfg.api_key = c.get_or("capability.stt.api_key", "");
  cfg.model = c.get_or("capability.stt.model", "FunAudioLLM/SenseVoiceSmall");
  cfg.language = c.get_or("capability.stt.language", "auto");
  return cfg;
}

TtsConfig load_tts_config(const fish::config::Config& c) {
  TtsConfig cfg;
  cfg.provider = c.get_or("capability.tts.provider", "edge");
  cfg.base_url = c.get_or("capability.tts.base_url", "");
  cfg.api_key = c.get_or("capability.tts.api_key", "");
  cfg.voice = c.get_or("capability.tts.voice", "zh-CN-XiaoyiNeural");
  cfg.format = c.get_or("capability.tts.format", "mp3");
  return cfg;
}

SearchConfig load_search_config(const fish::config::Config& c) {
  SearchConfig cfg;
  cfg.provider = c.get_or("capability.search.provider", "none");
  cfg.base_url = c.get_or("capability.search.base_url", "");
  cfg.api_key = c.get_or("capability.search.api_key", "");
  cfg.max_results = static_cast<int>(c.get_int_or("capability.search.max_results", 5));
  cfg.timeout_ms = static_cast<int>(c.get_int_or("capability.search.timeout_ms", 15000));
  cfg.search_need_content =
      c.get_bool_or("capability.search.need_content", false);
  cfg.fetch_max_chars =
      static_cast<std::size_t>(c.get_int_or("capability.search.fetch_max_chars", 6000));
  return cfg;
}

ImageConfig load_image_config(const fish::config::Config& c) {
  ImageConfig cfg;
  cfg.provider = c.get_or("capability.image.provider", "none");
  cfg.base_url = c.get_or("capability.image.base_url", "");
  cfg.api_key = c.get_or("capability.image.api_key", "");
  cfg.model = c.get_or("capability.image.model", "");
  cfg.default_size = c.get_or("capability.image.default_size", "1024x1024");
  cfg.timeout_ms = static_cast<int>(c.get_int_or("capability.image.timeout_ms", 120000));
  return cfg;
}

}  // namespace fish::capability
