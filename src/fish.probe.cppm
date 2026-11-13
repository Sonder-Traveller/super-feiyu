// src/fish.probe.cppm
// 上游能力自动探测：启动时（或缓存过期时）真实发一次请求，
// 判断上游支持哪些特性，结果写盘缓存。
//
// 为什么不用配置开关：
//   上游（尤其自建代理）会静默变更行为。写死配置要么过时、要么
//   需要人工改。实测 WorkBuddy 的 /v2/chat/completions 会在 HTTP 200
//   的情况下静默丢弃 assistant 角色的图片——这种缺陷只能靠探测发现。
//
// 缓存策略：
//   - 探测结果写 data/probe/capabilities.json
//   - 默认 30 天过期，过期后后台重探（不阻塞启动）
//   - 探测失败不覆盖已有结果（沿用上次成功的结论）
module;

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

export module fish.probe;

import fish.core;
import fish.log;
import fish.config;
import fish.capability;

export namespace fish::probe {
// 上游能力位。默认取保守值（不支持），探测成功后再打开。
struct Capabilities {
  // assistant 角色消息里带图片能否被模型看到
  bool assistant_image{false};
  // 是否支持 image_url 对象形式（OpenAI 标准）
  bool image_url_object{true};
  // 是否要求 stream=true（部分上游拒绝非流式）
  bool requires_stream{true};
  // 探测时间（unix 秒）与结果来源
  std::int64_t probed_at{0};
  std::string probed_model;
  std::string source{"default"};   // default | probe | cache
  // 探测失败时的原因（保留供排查）
  std::string last_error;
};

// 探测配置
struct Config {
  std::string cache_path{"data/probe/capabilities.json"};
  // 缓存有效期（天）。默认 30 天。
  int ttl_days{30};
  // 探测用的模型（空 = 用 chat 配置里的）
  std::string model;
  // 单次探测超时
  int timeout_ms{60000};
  // 是否在启动时同步探测（false = 只读缓存，后台再探）
  bool probe_on_start{true};
};

class CapabilityProbe {
 public:
  CapabilityProbe(Config cfg, std::string base_url, std::string api_key);
  ~CapabilityProbe();

  // 读取缓存。缓存不存在或过期时按 probe_on_start 决定是否同步探测。
  Capabilities load_or_probe();

  // 强制重新探测（忽略缓存）
  Capabilities probe_now();

  // 当前生效的能力（探测后更新）
  [[nodiscard]] Capabilities current() const;

  // 是否已过期（供后台线程判断）
  [[nodiscard]] bool stale() const;

 private:
  Capabilities probe_impl();
  bool load_cache(Capabilities* out) const;
  fish::core::Error save_cache(const Capabilities& caps) const;

  Config cfg_;
  std::string base_url_;
  std::string api_key_;
  mutable std::mutex mu_;
  Capabilities caps_;
};

}  // namespace fish::probe

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::probe {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace detail {

std::int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 1x1 红色 PNG，用于探测（极小，省流量）
constexpr const char* kProbeImage =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFAAH/"
    "q842iQAAAABJRU5ErkJggg==";

// 探测用的标准 OpenAI 请求体
json probe_body(const std::string& model, const json& messages) {
  return json{{"model", model}, {"messages", messages}, {"stream", true}};
}

}  // namespace detail

CapabilityProbe::CapabilityProbe(Config cfg, std::string base_url, std::string api_key)
    : cfg_(std::move(cfg)),
      base_url_(std::move(base_url)),
      api_key_(std::move(api_key)) {
  if (cfg_.model.empty()) cfg_.model = "deepseek-flash";
}

CapabilityProbe::~CapabilityProbe() = default;

Capabilities CapabilityProbe::current() const {
  std::lock_guard<std::mutex> lock(mu_);
  return caps_;
}

bool CapabilityProbe::stale() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (caps_.probed_at == 0) return true;
  const auto age_days = (detail::now_unix() - caps_.probed_at) / 86400;
  return age_days >= cfg_.ttl_days;
}

bool CapabilityProbe::load_cache(Capabilities* out) const {
  std::ifstream in(cfg_.cache_path);
  if (!in) return false;
  try {
    const auto j = json::parse(in);
    out->assistant_image = j.value("assistant_image", false);
    out->image_url_object = j.value("image_url_object", true);
    out->requires_stream = j.value("requires_stream", true);
    out->probed_at = j.value("probed_at", static_cast<std::int64_t>(0));
    out->probed_model = j.value("probed_model", "");
    out->source = "cache";
    out->last_error = j.value("last_error", "");
    return out->probed_at > 0;
  } catch (...) {
    return false;
  }
}

fish::core::Error CapabilityProbe::save_cache(const Capabilities& caps) const {
  std::error_code ec;
  const fs::path p(cfg_.cache_path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);

  json j{
      {"assistant_image", caps.assistant_image},
      {"image_url_object", caps.image_url_object},
      {"requires_stream", caps.requires_stream},
      {"probed_at", caps.probed_at},
      {"probed_model", caps.probed_model},
      {"last_error", caps.last_error},
  };

  const std::string tmp = cfg_.cache_path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                     "probe_cache_open");
    }
    out << j.dump(2) << "\n";
  }
  fs::rename(tmp, cfg_.cache_path, ec);
  if (ec) {
    return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                   "probe_cache_rename:" + ec.message());
  }
  return fish::core::Error::none();
}

Capabilities CapabilityProbe::load_or_probe() {
  Capabilities cached;
  const bool have_cache = load_cache(&cached);

  // P9 修复：多实例部署时避免重复探测。
  // 先看缓存是否新鲜——新鲜就直接用，不做任何网络请求。
  // 这样 N 个实例启动时只有一个会真正探测（其余读到它写的结果）。
  if (have_cache) {
    const auto age_days = (detail::now_unix() - cached.probed_at) / 86400;
    if (age_days < cfg_.ttl_days) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        caps_ = cached;
      }
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "probe", "cache_hit")
          .field("age_days", static_cast<long long>(age_days))
          .field("assistant_image", cached.assistant_image);
      return cached;
    }
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "probe", "cache_stale")
        .field("age_days", static_cast<long long>(age_days))
        .field("ttl_days", static_cast<long long>(cfg_.ttl_days));
  }

  if (cfg_.probe_on_start) {
    // 探测前再读一次缓存：可能别的实例刚刚探完并写了盘。
    // 这个二次检查把「N 个实例同时启动」的重复探测降到最多 1-2 次。
    Capabilities fresh;
    if (load_cache(&fresh)) {
      const auto age = (detail::now_unix() - fresh.probed_at) / 86400;
      if (age < cfg_.ttl_days) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          caps_ = fresh;
        }
        ::fish::log::log_line(::fish::log::LogLevel::kInfo, "probe", "cache_fresh_race")
            .field("age_days", static_cast<long long>(age));
        return fresh;
      }
    }
    return probe_now();
  }

  // 不同步探测：先用缓存（可能过期）或保守默认值
  if (have_cache) {
    std::lock_guard<std::mutex> lock(mu_);
    caps_ = cached;
    return cached;
  }
  std::lock_guard<std::mutex> lock(mu_);
  caps_.source = "default";
  return caps_;
}

Capabilities CapabilityProbe::probe_now() {
  auto caps = probe_impl();

  // 探测失败不覆盖已有成功结果
  if (!caps.last_error.empty()) {
    Capabilities prev;
    if (load_cache(&prev) && prev.probed_at > 0) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "probe", "keep_previous")
          .field("error", caps.last_error);
      std::lock_guard<std::mutex> lock(mu_);
      caps_ = prev;
      return prev;
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    caps_ = caps;
  }
  if (caps.last_error.empty()) {
    auto e = save_cache(caps);
    if (!e.ok()) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "probe", "cache_save_failed")
          .field("detail", e.detail);
    }
  }
  return caps;
}

Capabilities CapabilityProbe::probe_impl() {
  Capabilities caps;
  caps.probed_model = cfg_.model;
  caps.probed_at = detail::now_unix();

  const std::string url = base_url_ + "/chat/completions";
  std::map<std::string, std::string> headers{
      {"Authorization", "Bearer " + api_key_},
      {"Accept", "text/event-stream"},
  };

  // ── 探测 1：基础连通性 + 是否要求 stream ──
  {
    const auto body = detail::probe_body(
        cfg_.model,
        json::array({{{"role", "user"}, {"content", "1"}}})).dump();
    auto r = fish::capability::HttpClient::instance().post_json(
        url, headers, body, cfg_.timeout_ms);
    if (!r.ok()) {
      caps.last_error = "basic:" + r.error.detail;
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "probe", "basic_failed")
          .field("detail", r.error.detail);
      return caps;
    }
    // 上游若返回 SSE（data: 开头），说明 stream 生效
    caps.requires_stream = r.value->body.rfind("data:", 0) != std::string::npos;
  }

  // ── 探测 2：assistant 角色带图能否被看到 ──
  //
  // 手法：让 assistant「先发一张纯红色图」，再问 user「刚才那张图什么颜色」。
  //   - 上游支持 → 模型能答出红色
  //   - 上游丢弃 → 模型会说看不到/不知道，或胡乱猜
  // 用红/绿对照可以排除「蒙对」。
  {
    const auto body = detail::probe_body(cfg_.model, json::array({
        {{"role", "system"}, {"content", "只回答颜色词，不要解释。"}},
        {{"role", "assistant"}, {"content", json::array({
            {{"type", "text"}, {"text", "喏"}},
            {{"type", "image_url"},
             {"image_url", {{"url", detail::kProbeImage}}}},
        })}},
        {{"role", "user"}, {"content", "我刚才发的那张图是什么颜色？只回答颜色。"}},
    })).dump();

    auto r = fish::capability::HttpClient::instance().post_json(
        url, headers, body, cfg_.timeout_ms);
    if (r.ok()) {
      // 上游可能返回 SSE（data: {...}）也可能返回普通 JSON（桥接层会聚合）。
      // 两种都要能解析，否则探测结论会误判。
      std::string text;
      const std::string& raw = r.value->body;

      const bool looks_sse = raw.find("data:") != std::string::npos;
      if (looks_sse) {
        std::string line;
        std::istringstream ss(raw);
        while (std::getline(ss, line)) {
          const auto pos = line.find("data:");
          if (pos == std::string::npos) continue;
          const std::string chunk = line.substr(pos + 5);
          if (chunk.find("[DONE]") != std::string::npos) break;
          try {
            const auto d = json::parse(chunk);
            for (const auto& ch : d.value("choices", json::array())) {
              const auto delta = ch.value("delta", json::object());
              if (delta.contains("content") && delta["content"].is_string()) {
                text += delta["content"].get<std::string>();
              }
            }
          } catch (...) {
          }
        }
      } else {
        // 普通 JSON：桥接层聚合后的格式
        try {
          const auto d = json::parse(raw);
          for (const auto& ch : d.value("choices", json::array())) {
            const auto msg = ch.value("message", json::object());
            if (msg.contains("content") && msg["content"].is_string()) {
              text += msg["content"].get<std::string>();
            }
          }
        } catch (...) {
        }
      }
      // 判定：提到红色且没有否认词
      const bool says_red = text.find("红") != std::string::npos;
      const bool denies = text.find("没看到") != std::string::npos ||
                          text.find("看不到") != std::string::npos ||
                          text.find("没收到") != std::string::npos ||
                          text.find("没有图") != std::string::npos ||
                          text.find("无法") != std::string::npos;
      caps.assistant_image = says_red && !denies;
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "probe", "assistant_image")
          .field("supported", caps.assistant_image)
          .field("answer", text.substr(0, 60));
    } else {
      // 请求本身失败：可能是不接受该格式
      caps.assistant_image = false;
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "probe", "assistant_image")
          .field("supported", false)
          .field("reason", r.error.detail);
    }
  }

  caps.source = "probe";
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "probe", "done")
      .field("assistant_image", caps.assistant_image)
      .field("requires_stream", caps.requires_stream);
  return caps;
}

}  // namespace fish::probe
