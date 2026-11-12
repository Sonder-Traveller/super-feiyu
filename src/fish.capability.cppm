// src/fish.capability.cppm
// 远程能力：HTTP 基础设施 + 多 provider 统一接口。
// 本机零推理，此处全是网络调用。
module;
#include <algorithm>
#include <chrono>
#include <cstdio>
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

struct SearchConfig {
  std::string provider{"none"};
  std::string base_url;
  std::string api_key;
  int max_results{5};
  int timeout_ms{15000};
};

const char* chat_provider_list();
const char* embedding_provider_list();
const char* stt_provider_list();
const char* tts_provider_list();
const char* search_provider_list();

fish::core::Result<std::unique_ptr<fish::core::IChatModel>> make_chat(const ChatConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::IEmbeddingModel>> make_embedding(
    const EmbeddingConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::ISpeechToText>> make_stt(const SttConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::ITextToSpeech>> make_tts(const TtsConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::IWebSearch>> make_search(
    const SearchConfig& cfg);

ChatConfig load_chat_config(const fish::config::Config& c);
EmbeddingConfig load_embedding_config(const fish::config::Config& c);
SttConfig load_stt_config(const fish::config::Config& c);
TtsConfig load_tts_config(const fish::config::Config& c);
SearchConfig load_search_config(const fish::config::Config& c);

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
    for (const auto& h : req.history) {
      if (h.kind != fish::core::BlockKind::kText) continue;
      msgs.push_back({{"role", "user"}, {"content", h.text}});
    }
    msgs.push_back({{"role", "user"}, {"content", to_blocks(req.user_blocks)}});
    payload["messages"] = std::move(msgs);

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

    const std::string tmp = "/tmp/fish_stt_" + std::to_string(std::rand()) + ".audio";
    {
      std::ofstream f(tmp, std::ios::binary);
      if (!f) return Result<std::string>::fail(ErrorCode::kInternal, "tmp_write");
      f.write(audio.value->data(), static_cast<std::streamsize>(audio.value->size()));
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
    const std::string cmd = "edge-tts --voice '" + cfg_.voice + "' --text '" + safe +
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
          .field("status", result.status);
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
  return Result<std::unique_ptr<fish::core::IWebSearch>>::ok(
      std::make_unique<TavilySearch>(cfg));
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
  return cfg;
}

}  // namespace fish::capability
