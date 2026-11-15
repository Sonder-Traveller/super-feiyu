// src/fish.qq_ws.cppm
// QQ 官方机器人 WebSocket 协议栈。
//
// 协议要点（官方文档）：
//   op 10 HELLO          → 下发 heartbeat_interval，随后需发 IDENTIFY
//   op 2  IDENTIFY       → 携带 token + intents，鉴权
//   op 0  DISPATCH       → 事件（READY / GROUP_AT_MESSAGE_CREATE / C2C_MESSAGE_CREATE ...）
//   op 1  HEARTBEAT      → 定时发送，服务端回 ACK
//   op 11 HEARTBEAT_ACK  → 心跳确认
//   op 6  RESUME         → 带 session_id + seq 重连，避免丢事件
//   op 7  RECONNECT      → 服务端要求重连
//   op 9  INVALID_SESSION→ session 失效，需重新 IDENTIFY
//
// 本模块只负责协议与事件解码，不做业务判断。
module;
#include <algorithm>
#include <curl/curl.h>
#include <functional>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

export module fish.qq_ws;

import fish.core;
import fish.log;
import fish.ws;

export namespace fish::qq_ws {

// 订阅的 intent 位
enum class Intent : std::uint32_t {
  kGuilds = 1u << 0,
  kGuildMembers = 1u << 1,
  kGuildMessages = 1u << 9,
  kGroupAndC2C = 1u << 25,   // 群聊 + 私聊（v2 事件）
};

inline Intent operator|(Intent a, Intent b) {
  return static_cast<Intent>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
inline bool has_intent(std::uint32_t mask, Intent i) {
  return (mask & static_cast<std::uint32_t>(i)) != 0;
}

struct Config {
  std::string app_id;
  std::string app_secret;
  std::string api_base{"https://api.sgroup.qq.com"};
  std::string token_url{"https://bots.qq.com/app/getAppAccessToken"};
  std::uint32_t intents{static_cast<std::uint32_t>(Intent::kGroupAndC2C)};
  int token_refresh_lead_ms{120000};  // 提前 2 分钟刷新
};

// 解析 ISO8601 时间（形如 "2026-11-13T15:57:48+08:00"）为 unix 毫秒。
// 失败返回 0（调用方会退回本地接收时间）。
// 不引入第三方日期库：格式固定，手写解析够用且无依赖。
std::int64_t parse_iso8601_ms(const std::string& s) {
  // 最少需要 "YYYY-MM-DDTHH:MM:SS"
  if (s.size() < 19) return 0;
  std::tm tm{};
  int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
  if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &mon, &day, &hour, &min,
                  &sec) != 6) {
    return 0;
  }
  if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31) return 0;

  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_sec = sec;

  // timegm 按 UTC 解释；若带时区偏移需要补偿
  std::time_t t = ::timegm(&tm);
  if (t == static_cast<std::time_t>(-1)) return 0;

  // 处理时区偏移：+08:00 / -05:00 / Z
  const auto pos = s.find_last_of("Zz+-");
  if (pos != std::string::npos && pos > 18) {
    const char sign = s[pos];
    if (sign == '+' || sign == '-') {
      int oh = 0, om = 0;
      if (std::sscanf(s.c_str() + pos + 1, "%2d:%2d", &oh, &om) == 2 ||
          std::sscanf(s.c_str() + pos + 1, "%2d%2d", &oh, &om) == 2) {
        const std::int64_t offset = (static_cast<std::int64_t>(oh) * 3600 + om * 60) *
                                    (sign == '+' ? 1 : -1);
        t -= static_cast<std::time_t>(offset);
      }
    }
  }
  return static_cast<std::int64_t>(t) * 1000;
}

// 入站附件（QQ 官方把图片/语音放在 attachments 数组里）
struct Attachment {
  std::string content_type;   // image/jpeg、image/png、voice ...
  std::string url;            // 可直接下载的 URL（腾讯 CDN，带签名，有有效期）
  std::string filename;
  int size{0};
  int width{0};
  int height{0};
  [[nodiscard]] bool is_image() const {
    return content_type.rfind("image/", 0) == 0;
  }
  [[nodiscard]] bool is_voice() const {
    return content_type.rfind("voice", 0) == 0 ||
           content_type.rfind("audio/", 0) == 0 ||
           content_type == "silk";
  }
};

// 解析后的事件（只保留收发消息需要的信息）
struct Event {
  std::string type;              // GROUP_AT_MESSAGE_CREATE / C2C_MESSAGE_CREATE / READY
  std::string message_id;        // 被动回复凭证
  std::string conversation_id;   // 群：group_openid；私聊：user_openid（不带 "g:" 前缀）
  std::string sender_id;
  std::string sender_name;
  // 群名片（群内昵称，可能与全局昵称不同）
  std::string sender_group_card;
  // 群内角色："owner" | "admin" | "member"
  std::string sender_role;
  // 群名 / 会话标题（群聊才有）
  std::string conversation_title;
  // 消息发送时刻（unix 毫秒），来自事件里的 timestamp
  std::int64_t at_ms{0};
  std::string content;           // 已去掉 <@!xxx> 前缀
  bool is_group{false};
  bool mentioned{false};
  std::vector<Attachment> attachments;  // 图片 / 语音
};

class Client {
 public:
  using EventHandler = std::function<void(const Event&)>;

  explicit Client(Config cfg);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void on_event(EventHandler h) { on_event_ = std::move(h); }

  // 建立连接并开始收事件。返回后协议栈在后台运行。
  core::Error start();
  void stop();
  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] int event_count() const noexcept { return event_count_.load(); }
  [[nodiscard]] std::string session_id() const;

  // 供测试：处理一帧原始 JSON 文本
  void handle_raw_frame(const std::string& text);

 private:
  core::Error refresh_token();
  [[nodiscard]] std::string gateway_url() const;
  core::Error open_socket();
  void send_identify();
  void send_resume();
  void send_heartbeat();
  void heartbeat_loop();
  void schedule_reconnect();
  void reconnect_loop();
  void parse_dispatch(const std::string& type, const nlohmann::json& d);

  Config cfg_;
  EventHandler on_event_;

  mutable std::mutex mu_;
  std::string access_token_;
  core::TimePoint token_expiry_{};
  std::string session_id_;
  std::int64_t seq_{0};
  std::unique_ptr<ws::Client> socket_;
  int heartbeat_interval_ms_{41250};

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::atomic<int> event_count_{0};
  std::thread heartbeat_thread_;
  std::thread supervisor_thread_;
  std::atomic<int> backoff_ms_{1000};
};

}  // namespace fish::qq_ws

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::qq_ws {

using fish::core::Error;
using fish::core::ErrorCode;
using json = nlohmann::json;

namespace {

constexpr const char* kGatewayUrl = "wss://api.sgroup.qq.com/websocket";

// 去掉群消息里的 <@!bot_id> / <@bot_id> 前缀
std::string strip_mention(std::string s) {
  std::size_t pos = 0;
  while ((pos = s.find("<@", pos)) != std::string::npos) {
    const auto end = s.find('>', pos);
    if (end == std::string::npos) break;
    s.erase(pos, end - pos + 1);
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  }
  // 去掉首尾空白
  const auto not_space = [](unsigned char c) { return c != ' ' && c != '\t'; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

}  // namespace

Client::Client(Config cfg) : cfg_(std::move(cfg)) {}

Client::~Client() { stop(); }

std::string Client::session_id() const {
  std::lock_guard<std::mutex> lock(mu_);
  return session_id_;
}

Error Client::refresh_token() {
  json payload{{"appId", cfg_.app_id}, {"clientSecret", cfg_.app_secret}};
  // 直接用 curl 发一次（避免依赖 capability 模块造成循环）
  CURL* c = curl_easy_init();
  if (c == nullptr) return Error::make(ErrorCode::kInternal, "curl_init");

  std::string body = payload.dump();
  std::string resp;
  struct curl_slist* hs = nullptr;
  hs = curl_slist_append(hs, "Content-Type: application/json");

  curl_easy_setopt(c, CURLOPT_URL, cfg_.token_url.c_str());
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 15000L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                   +[](char* p, std::size_t sz, std::size_t n, void* u) -> std::size_t {
                     static_cast<std::string*>(u)->append(p, sz * n);
                     return sz * n;
                   });
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

  const CURLcode rc = curl_easy_perform(c);
  curl_slist_free_all(hs);
  curl_easy_cleanup(c);

  if (rc != CURLE_OK) {
    return Error::make(ErrorCode::kUpstreamUnavailable,
                       std::string("token_http:") + curl_easy_strerror(rc));
  }

  try {
    const auto j = json::parse(resp);
    if (!j.contains("access_token")) {
      return Error::make(ErrorCode::kUnauthorized, "token_missing_field");
    }
    std::lock_guard<std::mutex> lock(mu_);
    access_token_ = j["access_token"].get<std::string>();
    long long ttl = 7200;
    if (j.contains("expires_in")) {
      if (j["expires_in"].is_number()) {
        ttl = j["expires_in"].get<long long>();
      } else if (j["expires_in"].is_string()) {
        ttl = std::stoll(j["expires_in"].get<std::string>());
      }
    }
    token_expiry_ = core::Clock::now() +
                    std::chrono::seconds(ttl) -
                    std::chrono::milliseconds(cfg_.token_refresh_lead_ms);
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq_ws", "token_ok")
        .field("ttl_s", ttl);
    return Error::none();
  } catch (const std::exception& e) {
    return Error::make(ErrorCode::kBadRequest, std::string("token_parse:") + e.what());
  }
}

std::string Client::gateway_url() const { return kGatewayUrl; }

Error Client::open_socket() {
  ws::Config wc;
  wc.url = gateway_url();
  wc.connect_timeout_ms = 15000;
  wc.recv_timeout_ms = 1000;

  auto sock = std::make_unique<ws::Client>(wc);
  sock->on_message([this](const ws::Frame& f) { handle_raw_frame(f.payload); });
  sock->on_state([this](ws::State s) {
    if (s == ws::State::kClosed || s == ws::State::kFailed) {
      if (!stop_.load()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "qq_ws", "socket_closed");
        // 释放 socket 交给 supervisor 重连（它靠 socket_ == nullptr 判断）
        std::lock_guard<std::mutex> lock(mu_);
        if (socket_) {
          socket_->close();
          socket_.reset();
        }
        schedule_reconnect();
      }
    }
  });

  auto e = sock->connect();
  if (!e.ok()) return e;

  {
    std::lock_guard<std::mutex> lock(mu_);
    socket_ = std::move(sock);
  }
  return Error::none();
}

void Client::send_identify() {
  json payload;
  payload["op"] = 2;
  {
    std::lock_guard<std::mutex> lock(mu_);
    payload["d"] = {{"token", "QQBot " + access_token_},
                    {"intents", cfg_.intents},
                    {"shard", json::array({0, 1})},
                    {"properties", json::object()}};
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (socket_) {
    auto e = socket_->send_text(payload.dump());
    ::fish::log::log_line(e.ok() ? ::fish::log::LogLevel::kInfo
                                 : ::fish::log::LogLevel::kError,
                          "qq_ws", "identify_sent")
        .field("intents", static_cast<long long>(cfg_.intents));
  }
}

void Client::send_resume() {
  json payload;
  payload["op"] = 6;
  {
    std::lock_guard<std::mutex> lock(mu_);
    payload["d"] = {{"token", "QQBot " + access_token_},
                    {"session_id", session_id_},
                    {"seq", seq_}};
    if (socket_) {
      socket_->send_text(payload.dump());
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq_ws", "resume_sent")
          .field("seq", static_cast<long long>(seq_));
    }
  }
}

void Client::send_heartbeat() {
  json payload;
  payload["op"] = 1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    payload["d"] = seq_ == 0 ? json(nullptr) : json(seq_);
    if (socket_ && socket_->is_open()) socket_->send_text(payload.dump());
  }
}

void Client::heartbeat_loop() {
  while (!stop_.load()) {
    int interval = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      interval = heartbeat_interval_ms_;
    }
    // 分片睡眠，便于快速响应 stop
    const auto deadline = core::Clock::now() + std::chrono::milliseconds(interval);
    while (!stop_.load() && core::Clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (stop_.load()) break;
    send_heartbeat();
  }
}

void Client::schedule_reconnect() {
  // 由 supervisor 线程接管；这里只记录一次
  ::fish::log::log_line(::fish::log::LogLevel::kWarn, "qq_ws", "reconnect_needed")
      .field("backoff_ms", backoff_ms_.load());
}

void Client::reconnect_loop() {
  // 断线自愈：指数退避 1s → 60s，成功后重置。
  // 有 session_id 时优先 RESUME（避免丢事件），否则重新 IDENTIFY。
  while (!stop_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (stop_.load()) break;

    bool alive = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      alive = socket_ != nullptr;
    }
    if (alive) {
      backoff_ms_.store(1000);
      continue;
    }

    const int wait = backoff_ms_.load();
    for (int slept = 0; slept < wait && !stop_.load(); slept += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (stop_.load()) break;

    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "qq_ws", "reconnecting")
        .field("backoff_ms", wait);

    // token 过期就先刷新
    {
      bool need = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        need = access_token_.empty() || core::Clock::now() >= token_expiry_;
      }
      if (need) refresh_token();
    }

    auto e = open_socket();
    if (e.ok()) {
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq_ws", "reconnected");
      backoff_ms_.store(1000);
    } else {
      int next = wait * 2;
      if (next > 60000) next = 60000;
      backoff_ms_.store(next);
    }
  }
}

void Client::parse_dispatch(const std::string& type, const json& d) {
  ++event_count_;

  if (type == "READY") {
    std::lock_guard<std::mutex> lock(mu_);
    session_id_ = d.value("session_id", "");
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq_ws", "ready")
        .field("session", session_id_);
    return;
  }

  Event ev;
  ev.type = type;

  // 消息时间：事件里带 timestamp（ISO8601），转成 unix 毫秒。
  // 用事件时间而不是本地接收时间：网络延迟和补发消息都靠它才准。
  if (d.contains("timestamp") && d["timestamp"].is_string()) {
    ev.at_ms = parse_iso8601_ms(d["timestamp"].get<std::string>());
  }

  if (type == "GROUP_AT_MESSAGE_CREATE") {
    ev.is_group = true;
    ev.mentioned = true;  // 该事件本身就是 @ 机器人
    ev.message_id = d.value("id", "");
    const std::string group = d.value("group_openid", "");
    // 只放原始 openid：群聊前缀 "g:" 由 fish.session::session_key() 统一加，
    // 这里再加一次会得到 "g:g:<openid>"，与记忆/人格用的键错位。
    ev.conversation_id = group;
    if (d.contains("author") && d["author"].is_object()) {
      const auto& a = d["author"];
      ev.sender_id = a.value("id", "");
      // member_openid 是群内稳定标识，优先用它
      if (a.contains("member_openid") && a["member_openid"].is_string()) {
        ev.sender_id = a["member_openid"].get<std::string>();
      }
      ev.sender_name = a.value("username", ev.sender_id);
      // 群名片：群内显示名，可能与全局昵称不同
      if (a.contains("member_nick") && a["member_nick"].is_string()) {
        ev.sender_group_card = a["member_nick"].get<std::string>();
      }
    }
    // 群内角色：QQ 可能在 member_role 或 roles 里给
    if (d.contains("member_role") && d["member_role"].is_string()) {
      ev.sender_role = d["member_role"].get<std::string>();
    }
    ev.content = strip_mention(d.value("content", ""));
  } else if (type == "C2C_MESSAGE_CREATE") {
    ev.is_group = false;
    ev.mentioned = true;
    ev.message_id = d.value("id", "");
    ev.sender_id = d.value("author", json::object()).value("user_openid", "");
    if (ev.sender_id.empty() && d.contains("author")) {
      ev.sender_id = d["author"].value("id", "");
    }
    ev.conversation_id = ev.sender_id;
    ev.sender_name = ev.sender_id;
    if (d.contains("author") && d["author"].is_object()) {
      const auto& a = d["author"];
      // 私聊也尽量拿昵称
      if (a.contains("username") && a["username"].is_string()) {
        ev.sender_name = a["username"].get<std::string>();
      }
    }
    ev.content = strip_mention(d.value("content", ""));
  } else {
    return;  // 其他事件类型忽略
  }

  if (ev.message_id.empty()) return;

  // 附件：QQ 官方把图片/语音放在 attachments 数组，
  // content 里只有文字（纯图片消息时 content 为空）。
  if (d.contains("attachments") && d["attachments"].is_array()) {
    for (const auto& a : d["attachments"]) {
      if (!a.is_object()) continue;
      Attachment att;
      att.content_type = a.value("content_type", "");
      att.url = a.value("url", "");
      att.filename = a.value("filename", "");
      if (a.contains("size") && a["size"].is_number()) {
        att.size = a["size"].get<int>();
      }
      if (a.contains("width") && a["width"].is_number()) {
        att.width = a["width"].get<int>();
      }
      if (a.contains("height") && a["height"].is_number()) {
        att.height = a["height"].get<int>();
      }
      if (!att.url.empty()) ev.attachments.push_back(std::move(att));
    }
  }

  if (on_event_) {
    try {
      on_event_(ev);
    } catch (const std::exception& e) {
      ::fish::log::log_line(::fish::log::LogLevel::kError, "qq_ws", "handler_threw")
          .field("detail", e.what());
    }
  }
}

void Client::handle_raw_frame(const std::string& text) {
  if (text.empty()) return;
  json j;
  try {
    j = json::parse(text);
  } catch (...) {
    return;
  }

  const int op = j.value("op", -1);
  if (j.contains("s") && j["s"].is_number()) {
    std::lock_guard<std::mutex> lock(mu_);
    seq_ = j["s"].get<std::int64_t>();
  }

  switch (op) {
    case 10: {  // HELLO
      int interval = 41250;
      if (j.contains("d") && j["d"].is_object()) {
        interval = j["d"].value("heartbeat_interval", 41250);
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        heartbeat_interval_ms_ = interval;
      }
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq_ws", "hello")
          .field("heartbeat_ms", interval);

      // 有 session 尝试 RESUM(避免丢事件)，否则 IDENTIFY
      bool have_session = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        have_session = !session_id_.empty();
      }
      if (have_session) {
        send_resume();
      } else {
        send_identify();
      }
      return;
    }
    case 11:  // HEARTBEAT_ACK
      return;
    case 7:  // RECONNECT
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "qq_ws", "server_reconnect");
      return;
    case 9:  // INVALID_SESSION
      {
        std::lock_guard<std::mutex> lock(mu_);
        session_id_.clear();
        seq_ = 0;
      }
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "qq_ws", "invalid_session");
      send_identify();
      return;
    case 0: {  // DISPATCH
      const std::string t = j.value("t", "");
      if (j.contains("d")) parse_dispatch(t, j["d"]);
      return;
    }
    default:
      return;
  }
}

Error Client::start() {
  auto e = refresh_token();
  if (!e.ok()) return e;

  e = open_socket();
  if (!e.ok()) {
    running_.store(false);
    return e;
  }

  stop_.store(false);
  running_.store(true);
  heartbeat_thread_ = std::thread([this] { heartbeat_loop(); });
  supervisor_thread_ = std::thread([this] { reconnect_loop(); });

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq_ws", "started")
      .field("app_id", cfg_.app_id);
  return Error::none();
}

void Client::stop() {
  if (!running_.exchange(false) && !stop_.exchange(true)) return;
  stop_.store(true);

  if (heartbeat_thread_.joinable()) {
    if (heartbeat_thread_.get_id() != std::this_thread::get_id()) {
      heartbeat_thread_.join();
    } else {
      heartbeat_thread_.detach();
    }
  }
  if (supervisor_thread_.joinable()) {
    if (supervisor_thread_.get_id() != std::this_thread::get_id()) {
      supervisor_thread_.join();
    } else {
      supervisor_thread_.detach();
    }
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (socket_) {
      socket_->close();
      socket_.reset();
    }
  }
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq_ws", "stopped");
}

}  // namespace fish::qq_ws


