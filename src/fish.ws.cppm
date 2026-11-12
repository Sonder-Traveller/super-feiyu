// src/fish.ws.cppm
// 基于 libcurl 的 WebSocket 客户端。
// 不引入 Boost.Asio：libcurl 8.10+ 原生支持 WS，体积与依赖都更小。
//
// 线程模型：每个连接一个读线程（阻塞在 curl_ws_recv），
// 发送在调用方线程，内部用互斥锁保护 curl 句柄。
module;
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <curl/curl.h>

export module fish.ws;

import fish.core;
import fish.log;

export namespace fish::ws {

// 帧类型（与 libcurl 的 CURLWS_* 语义对齐）
struct Frame {
  std::string payload;
  bool binary{false};
  bool fin{true};
};

// 连接状态
enum class State : std::uint8_t { kIdle, kConnecting, kOpen, kClosed, kFailed };

const char* state_name(State s);

struct Config {
  std::string url;                       // wss://...
  std::vector<std::string> headers;      // 附加请求头（如 Authorization）
  int connect_timeout_ms{15000};
  int recv_timeout_ms{1000};             // 单次 recv 的轮询粒度
  bool auto_reconnect{true};
  int reconnect_base_ms{1000};
  int reconnect_max_ms{60000};
};

// WebSocket 客户端。
// 生命周期：connect() → 后台读线程持续收帧 → on_message 回调 → close()
class Client {
 public:
  using MessageHandler = std::function<void(const Frame&)>;
  using StateHandler = std::function<void(State)>;

  explicit Client(Config cfg);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void on_message(MessageHandler h) { on_message_ = std::move(h); }
  void on_state(StateHandler h) { on_state_ = std::move(h); }

  // 建立连接并启动读线程。阻塞直到握手完成或失败。
  core::Error connect();
  // 发送文本帧
  core::Error send_text(const std::string& text);
  // 主动关闭（幂等）
  void close();

  // 启动自愈监控：断开后按指数退避自动重连，直到 close()
  void enable_auto_reconnect();

  [[nodiscard]] State state() const noexcept { return state_.load(); }
  [[nodiscard]] bool is_open() const noexcept { return state_.load() == State::kOpen; }
  // 已收到的帧计数（观测用）
  [[nodiscard]] std::uint64_t received_frames() const noexcept { return received_; }
  [[nodiscard]] std::uint64_t sent_frames() const noexcept { return sent_; }

 private:
  void read_loop();
  void set_state(State s);
  core::Error do_connect_locked();
  void teardown_locked();

  Config cfg_;
  MessageHandler on_message_;
  StateHandler on_state_;

  mutable std::mutex mu_;          // 保护 curl_
  CURL* curl_{nullptr};
  std::atomic<State> state_{State::kIdle};
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> received_{0};
  std::atomic<std::uint64_t> sent_{0};
  std::thread reader_;
  std::thread supervisor_;
  std::atomic<bool> auto_reconnect_{false};
  void supervisor_loop();
};

}  // namespace fish::ws

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::ws {

namespace {

// 发送缓冲：curl_ws_send 可能只写一部分，需循环
bool send_all(CURL* c, const char* data, std::size_t len, bool binary) {
  std::size_t off = 0;
  int guard = 0;
  while (off < len) {
    std::size_t sent = 0;
    const CURLcode rc = curl_ws_send(c, data + off, len - off, &sent,
                                     /*totalsize=*/0, binary ? CURLWS_BINARY : CURLWS_TEXT);
    if (rc == CURLE_AGAIN) {
      // 缓冲满，稍等再试
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      if (++guard > 5000) return false;  // ~10s 上限
      continue;
    }
    if (rc != CURLE_OK) return false;
    off += sent;
    guard = 0;
    if (sent == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (++guard > 5000) return false;
    }
  }
  return true;
}

}  // namespace

const char* state_name(State s) {
  switch (s) {
    case State::kIdle: return "idle";
    case State::kConnecting: return "connecting";
    case State::kOpen: return "open";
    case State::kClosed: return "closed";
    case State::kFailed: return "failed";
  }
  return "unknown";
}

Client::Client(Config cfg) : cfg_(std::move(cfg)) {}

Client::~Client() { close(); }

void Client::set_state(State s) {
  state_.store(s);
  if (on_state_) on_state_(s);
}

core::Error Client::do_connect_locked() {
  if (curl_ != nullptr) {
    curl_easy_cleanup(curl_);
    curl_ = nullptr;
  }
  curl_ = curl_easy_init();
  if (curl_ == nullptr) {
    return core::Error::make(core::ErrorCode::kInternal, "curl_easy_init_failed");
  }

  curl_easy_setopt(curl_, CURLOPT_URL, cfg_.url.c_str());
  curl_easy_setopt(curl_, CURLOPT_CONNECT_ONLY, 2L);  // 2 = WebSocket
  curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(cfg_.connect_timeout_ms));
  curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 0L);  // 连接存活期不限
  curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
  // 长连接保活
  curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl_, CURLOPT_TCP_KEEPIDLE, 60L);
  curl_easy_setopt(curl_, CURLOPT_TCP_KEEPINTVL, 30L);

  curl_slist* hs = nullptr;
  for (const auto& h : cfg_.headers) hs = curl_slist_append(hs, h.c_str());
  if (hs != nullptr) {
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, hs);
    // 注意：curl 会在 cleanup 时释放 header 列表，
    // 因此把列表挂在 handle 上（用 CURLOPT_PRIVATE 记住以便清理）
    curl_easy_setopt(curl_, CURLOPT_PRIVATE, hs);
  }

  const CURLcode rc = curl_easy_perform(curl_);
  if (rc != CURLE_OK) {
    return core::Error::make(core::ErrorCode::kUpstreamUnavailable,
                             std::string("ws_connect:") + curl_easy_strerror(rc));
  }
  return core::Error::none();
}

core::Error Client::connect() {
  stop_.store(false);
  set_state(State::kConnecting);

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto e = do_connect_locked();
    if (!e.ok()) {
      set_state(State::kFailed);
      return e;
    }
  }

  set_state(State::kOpen);
  reader_ = std::thread([this] { read_loop(); });
  return core::Error::none();
}

void Client::read_loop() {
  std::vector<char> buf(1 << 20);  // 1MB 单帧上限
  int consecutive_errors = 0;

  while (!stop_.load()) {
    CURL* c = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      c = curl_;
    }
    if (c == nullptr) break;

    std::size_t n = 0;
    const struct curl_ws_frame* meta = nullptr;
    CURLcode rc;
    {
      // curl_ws_recv 需与发送串行化，但不该长期持锁；
      // 这里只做一次短读，锁粒度足够小
      std::lock_guard<std::mutex> lock(mu_);
      rc = curl_ws_recv(c, buf.data(), buf.size(), &n, &meta);
    }

    if (rc == CURLE_AGAIN) {
      // 无数据：短睡眠避免空转
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (rc == CURLE_GOT_NOTHING) {
      if (!stop_.load()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "ws", "peer_closed").field("url", cfg_.url);
        set_state(State::kClosed);
      }
      break;
    }
    if (rc != CURLE_OK) {
      if (stop_.load()) break;
      ++consecutive_errors;
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "ws", "recv_failed")
          .field("url", cfg_.url)
          .field("detail", curl_easy_strerror(rc))
          .field("count", consecutive_errors);
      if (consecutive_errors > 3) {
        set_state(State::kFailed);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    consecutive_errors = 0;
    if (n > 0 && meta != nullptr) {
      Frame f;
      f.payload.assign(buf.data(), n);
      f.binary = (meta->flags & CURLWS_BINARY) != 0;
      f.fin = (meta->flags & CURLWS_CONT) == 0;
      ++received_;
      if (on_message_) {
        try {
          on_message_(f);
        } catch (const std::exception& e) {
          ::fish::log::log_line(::fish::log::LogLevel::kError, "ws", "handler_threw").field("detail", e.what());
        }
      }
    }
  }
  set_state(State::kClosed);
}

core::Error Client::send_text(const std::string& text) {
  if (!is_open()) {
    return core::Error::make(core::ErrorCode::kUpstreamUnavailable, "ws_not_open");
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (curl_ == nullptr) {
    return core::Error::make(core::ErrorCode::kInternal, "ws_handle_null");
  }
  if (!send_all(curl_, text.data(), text.size(), /*binary=*/false)) {
    return core::Error::make(core::ErrorCode::kUpstreamUnavailable, "ws_send_failed");
  }
  ++sent_;
  return core::Error::none();
}

void Client::teardown_locked() {
  if (curl_ != nullptr) {
    void* priv = nullptr;
    curl_easy_getinfo(curl_, CURLINFO_PRIVATE, &priv);
    if (priv != nullptr) {
      curl_slist_free_all(static_cast<curl_slist*>(priv));
    }
    curl_easy_cleanup(curl_);
    curl_ = nullptr;
  }
}

void Client::enable_auto_reconnect() {
  if (auto_reconnect_.exchange(true)) return;
  supervisor_loop_setup:
  supervisor_ = std::thread([this] { supervisor_loop(); });
}

void Client::supervisor_loop() {
  int backoff_ms = cfg_.reconnect_base_ms > 0 ? cfg_.reconnect_base_ms : 1000;

  while (!stop_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (stop_.load()) break;

    if (is_open()) {
      backoff_ms = cfg_.reconnect_base_ms > 0 ? cfg_.reconnect_base_ms : 1000;
      continue;
    }

    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "ws", "reconnect_begin").field("url", cfg_.url);

    // 清理旧连接
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) {
      reader_.join();
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      teardown_locked();
    }

    // 退避等待
    const int wait = backoff_ms;
    for (int slept = 0; slept < wait && !stop_.load(); slept += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (stop_.load()) break;

    set_state(State::kConnecting);
    core::Error e;
    {
      std::lock_guard<std::mutex> lock(mu_);
      e = do_connect_locked();
    }
    if (e.ok()) {
      set_state(State::kOpen);
      reader_ = std::thread([this] { read_loop(); });
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "ws", "reconnect_ok").field("url", cfg_.url);
      backoff_ms = cfg_.reconnect_base_ms > 0 ? cfg_.reconnect_base_ms : 1000;
    } else {
      set_state(State::kFailed);
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "ws", "reconnect_failed").field("url", cfg_.url).field("backoff_ms", backoff_ms);
      backoff_ms *= 2;
      const int cap = cfg_.reconnect_max_ms > 0 ? cfg_.reconnect_max_ms : 60000;
      if (backoff_ms > cap) backoff_ms = cap;
    }
  }
}
void Client::close() {
  if (stop_.exchange(true)) return;

  // 先停自愈监控，避免它在关闭过程中重连
  if (supervisor_.joinable() && supervisor_.get_id() != std::this_thread::get_id()) {
    supervisor_.join();
  }

  // 再让读线程退出（它靠 stop_ 与短读超时收敛）
  if (reader_.joinable()) {
    if (reader_.get_id() != std::this_thread::get_id()) {
      reader_.join();
    } else {
      reader_.detach();
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    teardown_locked();
  }
  state_.store(State::kClosed);
}

}  // namespace fish::ws






