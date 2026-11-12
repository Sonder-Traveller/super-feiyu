// src/fish.onebot_ws.cppm
// OneBot v11 WebSocket 客户端（支持正向连接与反向连接的事件接收）。
//
// 正向：我们作为客户端连 ws://127.0.0.1:3001（NapCat/Lagrange 的 WS 服务端）
// 反向：NapCat 主动连我们（需独立的 WS 服务端，此处不实现，仅支持正向）
//
// 本模块只做协议收发与事件解码，不做业务判断。
module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

export module fish.onebot_ws;

import fish.core;
import fish.log;
import fish.ws;

export namespace fish::onebot_ws {

struct Config {
  std::string ws_url{"ws://127.0.0.1:3001"};
  std::string access_token;   // 可选，作为 Authorization 头发送
  std::string self_id;        // 机器人 QQ 号
  int connect_timeout_ms{15000};
};

// 消息段（只保留用得到的）
struct Segment {
  std::string type;   // text / at / image / record / face / reply
  std::string text;   // text 的正文
  std::string url;    // image/record 的可下载地址
  std::string file;   // 原始文件标识
  std::string target; // at 的 qq 号
};

struct Event {
  bool is_group{false};
  std::string group_id;
  std::string user_id;
  std::string message_id;
  std::string nickname;
  std::string raw_message;
  std::vector<Segment> segments;
  bool mentioned{false};
  std::int64_t time{0};
};

class Client {
 public:
  using EventHandler = std::function<void(const Event&)>;

  explicit Client(Config cfg);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void on_event(EventHandler h) { on_event_ = std::move(h); }

  core::Error start();
  void stop();
  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] int event_count() const noexcept { return event_count_.load(); }
  [[nodiscard]] bool connected() const noexcept;

  // 供测试：直接喂一个 OneBot 事件 JSON
  core::Result<Event> parse_event(const std::string& raw_json) const;
  void handle_raw_frame(const std::string& text);

 private:
  void parse_and_dispatch(const std::string& text);

  Config cfg_;
  EventHandler on_event_;
  std::unique_ptr<ws::Client> socket_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::atomic<int> event_count_{0};
};

}  // namespace fish::onebot_ws

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::onebot_ws {

using fish::core::Error;
using fish::core::ErrorCode;
using fish::core::Result;
using json = nlohmann::json;

Client::Client(Config cfg) : cfg_(std::move(cfg)) {}
Client::~Client() { stop(); }

bool Client::connected() const noexcept {
  return socket_ != nullptr && socket_->is_open();
}

Result<Event> Client::parse_event(const std::string& raw_json) const {
  json j;
  try {
    j = json::parse(raw_json);
  } catch (const std::exception& e) {
    return Result<Event>::fail(ErrorCode::kBadRequest, std::string("bad_json:") + e.what());
  }

  if (j.value("post_type", "") != "message") {
    return Result<Event>::fail(ErrorCode::kUnsupported, "not_message");
  }

  Event ev;
  ev.time = j.value("time", 0LL);
  const std::string mt = j.value("message_type", "");
  ev.is_group = (mt == "group");
  ev.user_id = std::to_string(j.value("user_id", 0LL));
  ev.message_id = std::to_string(j.value("message_id", 0LL));
  if (ev.is_group) ev.group_id = std::to_string(j.value("group_id", 0LL));
  ev.raw_message = j.value("raw_message", "");

  if (j.contains("sender") && j["sender"].is_object()) {
    ev.nickname = j["sender"].value("card", "");
    if (ev.nickname.empty()) ev.nickname = j["sender"].value("nickname", ev.user_id);
  } else {
    ev.nickname = ev.user_id;
  }

  bool mentioned = false;
  if (j.contains("message") && j["message"].is_array()) {
    for (const auto& seg : j["message"]) {
      Segment s;
      s.type = seg.value("type", "");
      const auto& d = seg.contains("data") ? seg["data"] : json::object();

      if (s.type == "text") {
        s.text = d.value("text", "");
        if (s.text.empty()) continue;
      } else if (s.type == "at") {
        s.target = d.value("qq", "");
        // @机器人 或 @全体 之外的目标
        if (!cfg_.self_id.empty() && s.target == cfg_.self_id) mentioned = true;
      } else if (s.type == "image") {
        s.url = d.value("url", "");
        s.file = d.value("file", "");
      } else if (s.type == "record") {
        s.url = d.value("url", "");
        s.file = d.value("file", "");
      } else if (s.type == "reply") {
        s.target = d.value("id", "");
      } else {
        // face 等其他段忽略
        continue;
      }
      ev.segments.push_back(std::move(s));
    }
  } else if (!ev.raw_message.empty()) {
    // 退化：只有 CQ 码字符串
    Segment s;
    s.type = "text";
    s.text = ev.raw_message;
    ev.segments.push_back(std::move(s));
  }

  ev.mentioned = mentioned;

  // 过滤掉纯元数据段（at / reply）：它们本身不构成可处理内容。
  // 只有 at 没有文本的消息若放行，会触发一次无意义的空回复。
  ev.segments.erase(
      std::remove_if(ev.segments.begin(), ev.segments.end(),
                     [](const Segment& s) { return s.type == "at" || s.type == "reply"; }),
      ev.segments.end());

  if (ev.segments.empty()) {
    return Result<Event>::fail(ErrorCode::kBadRequest, "empty_message");
  }
  return Result<Event>::ok(std::move(ev));
}

void Client::handle_raw_frame(const std::string& text) { parse_and_dispatch(text); }

void Client::parse_and_dispatch(const std::string& text) {
  auto r = parse_event(text);
  if (!r.ok()) {
    // 非消息事件（心跳、生命周期、notice）静默忽略
    return;
  }
  ++event_count_;
  if (on_event_) {
    try {
      on_event_(*r.value);
    } catch (const std::exception& e) {
      ::fish::log::log_line(::fish::log::LogLevel::kError, "onebot", "handler_threw")
          .field("detail", e.what());
    }
  }
}

Error Client::start() {
  ws::Config wc;
  wc.url = cfg_.ws_url;
  wc.connect_timeout_ms = cfg_.connect_timeout_ms;
  if (!cfg_.access_token.empty()) {
    wc.headers.push_back("Authorization: Bearer " + cfg_.access_token);
  }

  auto sock = std::make_unique<ws::Client>(wc);
  sock->on_message([this](const ws::Frame& f) { handle_raw_frame(f.payload); });
  sock->on_state([this](ws::State s) {
    if ((s == ws::State::kClosed || s == ws::State::kFailed) && !stop_.load()) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "onebot", "socket_closed")
          .field("url", cfg_.ws_url);
    }
  });

  // 断线自愈：服务端重启或网络抖动后自动重连
  sock->enable_auto_reconnect();

  auto e = sock->connect();
  if (!e.ok()) {
    ::fish::log::log_line(::fish::log::LogLevel::kError, "onebot", "connect_failed")
        .field("url", cfg_.ws_url)
        .field("detail", e.detail);
    return e;
  }

  socket_ = std::move(sock);
  stop_.store(false);
  running_.store(true);
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "onebot", "started")
      .field("url", cfg_.ws_url);
  return Error::none();
}

void Client::stop() {
  if (stop_.exchange(true)) return;
  running_.store(false);
  if (socket_) {
    socket_->close();
    socket_.reset();
  }
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "onebot", "stopped");
}

}  // namespace fish::onebot_ws



