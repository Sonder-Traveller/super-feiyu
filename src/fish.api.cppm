// 控制面 API：把人格管理暴露成 HTTP 接口，由另一个进程调用。
//
// 只对外暴露 API，不带 WebUI。设计取舍：
//   - 只监听 127.0.0.1（除非配置显式改成 0.0.0.0），默认不对外网开放
//   - 必须带 Bearer token，token 从环境变量读，不落配置文件
//   - 只支持几个明确的端点，不实现通用路由
//   - 单线程 accept + 每连接短处理：控制面请求量极小，不值得上线程池
module;

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

export module fish.api;

import fish.core;
import fish.log;
import fish.persona;
import fish.control;

export namespace fish::api {

// 一条 HTTP 请求（够用即可，不追求完整实现）
struct Request {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;  // 键统一转小写
  std::string body;
};

struct Response {
  int status{200};
  std::string body;
  std::string content_type{"application/json; charset=utf-8"};
};

// 鉴权配置
struct AuthConfig {
  // 是否启用鉴权。false = 不做任何校验（仅供本机调试）。
  bool enabled{true};
  // Bearer token。由 main 从环境变量读入，不写死在这里。
  std::string token;
};

class Server {
 public:
  struct Config {
    std::string bind_addr{"127.0.0.1"};
    int port{0};          // 0 = 不启动
    AuthConfig auth;
    // 版本号与启动时刻，/healthz 与 /v1/status 要报
    std::string version{"0.2.0"};
  };

  using Handler = std::function<Response(const Request&)>;

  Server(Config cfg, fish::persona::PersonaStore* store,
         fish::control::ControlService* control = nullptr);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // 启动监听线程。port=0 时直接返回 ok 且不启动。
  fish::core::Error start();
  void stop();

  [[nodiscard]] bool running() const noexcept { return running_; }
  [[nodiscard]] int port() const noexcept { return actual_port_; }

  // 注册额外路由（保留扩展位）
  void route(const std::string& method, const std::string& path, Handler h);

  // 注册运行时状态回调，供 /v1/status 返回通道连接情况。
  // 用回调而不是持有 dispatcher：api 不该知道 dispatcher 存在。
  void set_status_provider(std::function<std::string()> cb) {
    status_cb_ = std::move(cb);
  }

  // 注册重启回调。POST /v1/restart 时调用（异步线程里），
  // main 里实现成 raise(SIGTERM)，让 systemd Restart=always 接住拉起新进程。
  void set_restart_callback(std::function<void()> cb) { restart_cb_ = std::move(cb); }

 private:
  void accept_loop();
  void handle_connection(int fd);
  Response dispatch(const Request& req);
  Response builtin_route(const Request& req);
  bool check_auth(const Request& req) const;

  Config cfg_;
  fish::persona::PersonaStore* store_{nullptr};
  fish::control::ControlService* control_{nullptr};
  std::map<std::string, Handler> routes_;   // "GET /x" → handler
  std::mutex routes_mu_;
  std::function<std::string()> status_cb_;
  std::function<void()> restart_cb_;

  // 启动时刻，用来算 uptime
  std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};

  int listen_fd_{-1};
  int actual_port_{0};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

// ── 解析/构造工具（导出便于测试）────────────────────────

// 从原始 HTTP 请求文本解析出 Request。解析失败返回 false。
bool parse_request(const std::string& raw, Request* out);

// 把 Response 序列化成完整 HTTP 响应。
std::string build_response(const Response& resp);

// 常量时间比较，避免 token 比较被计时攻击。
bool constant_time_equal(const std::string& a, const std::string& b);

}  // namespace fish::api

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::api {

namespace {

std::string to_lower(std::string s) {
  for (auto& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// 极简 JSON 字符串转义
std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// 从简单 JSON 对象里取一个字符串字段。
// 只处理 {"key":"value"} 这种扁平结构——控制面参数就是这么多，
// 上完整 JSON 解析器不值当。value 支持常规转义，包括 \uXXXX。
bool json_get_string(const std::string& json, const std::string& key, std::string* out) {
  const std::string needle = "\"" + key + "\"";
  auto pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos = json.find('"', pos);
  if (pos == std::string::npos) return false;

  // 把 \uXXXX 的一个码元转成数值，失败返回 -1
  const auto hex4 = [&json](std::size_t at) -> long {
    if (at + 4 > json.size()) return -1;
    long v = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      const char h = json[at + k];
      v <<= 4;
      if (h >= '0' && h <= '9') v |= (h - '0');
      else if (h >= 'a' && h <= 'f') v |= (h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') v |= (h - 'A' + 10);
      else return -1;
    }
    return v;
  };

  // 码点 → UTF-8
  const auto append_utf8 = [](std::string& s, unsigned cp) {
    if (cp <= 0x7F) {
      s += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
      s += static_cast<char>(0xC0 | (cp >> 6));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
      s += static_cast<char>(0xE0 | (cp >> 12));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      s += static_cast<char>(0xF0 | (cp >> 18));
      s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    }
  };

  std::string val;
  for (std::size_t i = pos + 1; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '\\' && i + 1 < json.size()) {
      const char n = json[++i];
      switch (n) {
        case 'n': val += '\n'; break;
        case 'r': val += '\r'; break;
        case 't': val += '\t'; break;
        case 'b': val += '\b'; break;
        case 'f': val += '\f'; break;
        case '"': val += '"'; break;
        case '\\': val += '\\'; break;
        case '/': val += '/'; break;
        case 'u': {
          // 关键：早期没处理 \u，`default: val += n` 把 'u' 当普通字符加进去，
          // "\u4e2d" 变成字面量 "u4e2d" —— 中文全部变成乱码。
          const long hi = hex4(i + 1);
          if (hi < 0) {
            val += 'u';   // 非法转义，原样保留
            break;
          }
          i += 4;
          unsigned cp = static_cast<unsigned>(hi);
          // 代理对：\uD800-\uDBFF 后面必须跟 \uDC00-\uDFFF
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < json.size() &&
              json[i + 1] == '\\' && json[i + 2] == 'u') {
            const long lo = hex4(i + 3);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + static_cast<unsigned>(lo - 0xDC00);
              i += 6;
            }
          }
          append_utf8(val, cp);
          break;
        }
        default: val += n;
      }
      continue;
    }
    if (c == '"') {
      *out = std::move(val);
      return true;
    }
    val += c;
  }
  return false;
}

// URL 里取查询参数值（不做 %XX 解码，控制面够用）
std::string query_param(const std::string& path, const std::string& key) {
  const auto q = path.find('?');
  if (q == std::string::npos) return {};
  const std::string qs = path.substr(q + 1);
  std::size_t start = 0;
  while (start <= qs.size()) {
    const auto amp = qs.find('&', start);
    const auto piece = qs.substr(start, amp == std::string::npos ? std::string::npos
                                                                 : amp - start);
    const auto eq = piece.find('=');
    if (eq != std::string::npos && piece.substr(0, eq) == key) {
      return piece.substr(eq + 1);
    }
    if (amp == std::string::npos) break;
    start = amp + 1;
  }
  return {};
}

std::string path_only(const std::string& path) {
  const auto q = path.find('?');
  return q == std::string::npos ? path : path.substr(0, q);
}

}  // namespace

// ── 解析 ────────────────────────────────────────────────

bool constant_time_equal(const std::string& a, const std::string& b) {
  // 长度不同也要走完全程，避免按长度泄露信息
  unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
  const std::size_t n = a.size() > b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    diff = static_cast<unsigned char>(diff | (ca ^ cb));
  }
  return diff == 0;
}

bool parse_request(const std::string& raw, Request* out) {
  const auto hdr_end = raw.find("\r\n\r\n");
  if (hdr_end == std::string::npos) return false;

  std::istringstream hs(raw.substr(0, hdr_end));
  std::string line;
  if (!std::getline(hs, line)) return false;
  if (!line.empty() && line.back() == '\r') line.pop_back();

  std::istringstream first(line);
  if (!(first >> out->method >> out->path)) return false;

  while (std::getline(hs, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    out->headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
  }

  out->body = raw.substr(hdr_end + 4);
  return true;
}

std::string build_response(const Response& resp) {
  std::ostringstream os;
  os << "HTTP/1.1 " << resp.status << ' '
     << (resp.status == 200 ? "OK"
         : resp.status == 201 ? "Created"
         : resp.status == 400 ? "Bad Request"
         : resp.status == 401 ? "Unauthorized"
         : resp.status == 404 ? "Not Found"
         : resp.status == 405 ? "Method Not Allowed"
         : resp.status == 413 ? "Payload Too Large"
         : "Internal Server Error")
     << "\r\n";
  os << "Content-Type: " << resp.content_type << "\r\n";
  os << "Content-Length: " << resp.body.size() << "\r\n";
  os << "Connection: close\r\n\r\n";
  os << resp.body;
  return os.str();
}

// ── Server ──────────────────────────────────────────────

Server::Server(Config cfg, fish::persona::PersonaStore* store,
               fish::control::ControlService* control)
    : cfg_(std::move(cfg)), store_(store), control_(control) {}

Server::~Server() { stop(); }

void Server::route(const std::string& method, const std::string& path, Handler h) {
  std::lock_guard<std::mutex> lock(routes_mu_);
  routes_[to_lower(method) + " " + path] = std::move(h);
}

fish::core::Error Server::start() {
  if (cfg_.port <= 0) {
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "api", "disabled")
        .field("reason", "port=0");
    return fish::core::Error::none();
  }

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return fish::core::Error::make(fish::core::ErrorCode::kInternal, "socket_failed");
  }

  const int yes = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(cfg_.port));
  if (::inet_pton(AF_INET, cfg_.bind_addr.c_str(), &addr.sin_addr) != 1) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return fish::core::Error::make(fish::core::ErrorCode::kBadRequest,
                                   "bad_bind_addr:" + cfg_.bind_addr);
  }

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    const std::string err = std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                   "bind_failed:" + err);
  }

  if (::listen(listen_fd_, 16) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return fish::core::Error::make(fish::core::ErrorCode::kInternal, "listen_failed");
  }

  // port=0 表示让系统分配，回读真实端口（测试很有用）
  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
    actual_port_ = ntohs(bound.sin_port);
  } else {
    actual_port_ = cfg_.port;
  }

  running_ = true;
  thread_ = std::thread([this] { accept_loop(); });

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "api", "listening")
      .field("addr", cfg_.bind_addr)
      .field("port", static_cast<long long>(actual_port_))
      .field("auth", cfg_.auth.enabled);
  return fish::core::Error::none();
}

void Server::stop() {
  if (!running_.exchange(false)) return;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "api", "stopped");
}

void Server::accept_loop() {
  while (running_) {
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (fd < 0) {
      if (!running_) break;
      continue;
    }
    handle_connection(fd);
    ::close(fd);
  }
}

bool Server::check_auth(const Request& req) const {
  if (!cfg_.auth.enabled) return true;
  if (cfg_.auth.token.empty()) return false;

  const auto it = req.headers.find("authorization");
  if (it == req.headers.end()) return false;

  const std::string& v = it->second;
  const std::string prefix = "bearer ";
  if (v.size() <= prefix.size()) return false;
  if (to_lower(v.substr(0, prefix.size())) != prefix) return false;
  return constant_time_equal(v.substr(prefix.size()), cfg_.auth.token);
}

Response Server::builtin_route(const Request& req) {
  const std::string path = path_only(req.path);

  // ── 健康检查与版本 ──
  // /healthz 在 dispatch 里已处理（免鉴权），这里不再重复。
  if (req.method == "GET" && path == "/v1/status") {
    const auto up = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - started_at_)
                        .count();
    std::string body = R"({"version":")" + json_escape(cfg_.version) +
                       R"(","uptime_s":)" + std::to_string(up) + R"(,"channels":)";
    body += status_cb_ ? status_cb_() : "[]";
    body += "}";
    Response r;
    r.body = std::move(body);
    return r;
  }

  // ── 配置读写 ──
  if (control_ != nullptr) {
    if (req.method == "GET" && path == "/v1/config") {
      auto j = control_->config_json();
      Response r;
      if (!j.ok()) {
        r.status = 500;
        r.body = R"({"error":"config_read_failed","detail":")" +
                 json_escape(j.error.detail) + R"("})";
        return r;
      }
      r.body = *j.value;
      return r;
    }

    if (req.method == "PUT" && path == "/v1/config") {
      std::vector<std::string> changed;
      bool restart = false;
      auto e = control_->apply_config_json(req.body, &changed, &restart);
      Response r;
      if (!e.ok()) {
        r.status = (e.code == fish::core::ErrorCode::kBadRequest) ? 400 : 500;
        std::string items;
        for (const auto& c : changed) {
          if (!items.empty()) items += ",";
          items += R"(")" + json_escape(c) + R"(")";
        }
        r.body = R"({"error":"config_write_failed","detail":")" +
                 json_escape(e.detail) + R"(","changed":[)" + items + "]}";
        return r;
      }
      std::string items;
      for (const auto& c : changed) {
        if (!items.empty()) items += ",";
        items += R"(")" + json_escape(c) + R"(")";
      }
      r.body = std::string(R"({"ok":true,"changed":[)") + items +
               R"(],"restart_required":)" + (restart ? "true" : "false") + "}";
      return r;
    }

    // ── 渠道测试 ──
    if (req.method == "POST" && path == "/v1/channel/qq/test") {
      std::string app_id, app_secret;
      json_get_string(req.body, "app_id", &app_id);
      json_get_string(req.body, "app_secret", &app_secret);
      if (app_secret == "__keep__") app_secret.clear();
      auto j = control_->qq_test(app_id, app_secret);
      Response r;
      if (!j.ok()) {
        r.status = 502;
        r.body = R"({"ok":false,"error":"upstream","detail":")" +
                 json_escape(j.error.detail) + R"("})";
        return r;
      }
      r.body = *j.value;
      return r;
    }

    if (req.method == "POST" && path == "/v1/channel/wechat/test") {
      std::string tok;
      json_get_string(req.body, "bot_token", &tok);
      if (tok == "__keep__") tok.clear();
      auto j = control_->wechat_test(tok);
      Response r;
      if (!j.ok()) {
        r.status = 502;
        r.body = R"({"ok":false,"error":"upstream","detail":")" +
                 json_escape(j.error.detail) + R"("})";
        return r;
      }
      r.body = *j.value;
      return r;
    }

    // ── QQ 扫码 ──
    if (req.method == "POST" && path == "/v1/channel/qq/qr") {
      std::string source;
      json_get_string(req.body, "source", &source);
      auto j = control_->qq_qr_create(source);
      Response r;
      if (!j.ok()) {
        r.status = 502;
        r.body = R"({"error":"qr_create_failed","detail":")" +
                 json_escape(j.error.detail) + R"("})";
        return r;
      }
      r.body = *j.value;
      return r;
    }

    if (req.method == "GET" && path == "/v1/channel/qq/qr/status") {
      auto j = control_->qq_qr_status(query_param(req.path, "task_id"));
      Response r;
      if (!j.ok()) {
        r.status = 502;
        r.body = R"({"status":"error","detail":")" + json_escape(j.error.detail) + R"("})";
        return r;
      }
      r.body = *j.value;
      return r;
    }

    // ── 微信扫码 ──
    if (req.method == "POST" && path == "/v1/channel/wechat/qr") {
      auto j = control_->wechat_qr_create();
      Response r;
      if (!j.ok()) {
        r.status = 502;
        r.body = R"({"error":"qr_create_failed","detail":")" +
                 json_escape(j.error.detail) + R"("})";
        return r;
      }
      r.body = *j.value;
      return r;
    }

    if (req.method == "GET" && path == "/v1/channel/wechat/qr/status") {
      auto j = control_->wechat_qr_status(query_param(req.path, "qrcode"));
      Response r;
      if (!j.ok()) {
        r.status = 502;
        r.body = R"({"status":"error","detail":")" + json_escape(j.error.detail) + R"("})";
        return r;
      }
      r.body = *j.value;
      return r;
    }
    // ── 重启服务 ──
    // 扫码绑定/改配置后，通道凭据要重启进程才生效。GUI 调这个接口
    // 触发优雅退出（SIGTERM），systemd Restart=always 拉起新进程。
    if (req.method == "POST" && path == "/v1/restart") {
      Response r;
      r.body = R"({"ok":true,"detail":"restarting"})";
      if (restart_cb_) {
        // 先应答再重启，否则客户端收不到响应
        auto cb = restart_cb_;
        std::thread([cb]() {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          cb();
        }).detach();
      }
      return r;
    }
  }

  if (store_ == nullptr) {
    Response r;
    r.status = 500;
    r.body = R"({"error":"persona_store_unavailable"})";
    return r;
  }

  // GET /v1/persona/default
  if (req.method == "GET" && path == "/v1/persona/default") {
    auto p = store_->default_persona();
    Response r;
    if (!p.ok()) {
      r.status = 500;
      r.body = R"({"error":"read_failed"})";
      return r;
    }
    r.body = R"({"persona": ")" + json_escape(*p.value) + R"("})";
    return r;
  }

  // PUT /v1/persona/default  {"persona":"..."}
  if (req.method == "PUT" && path == "/v1/persona/default") {
    std::string text;
    // 字段必须存在：缺字段或坏 JSON 时若当成空串处理，
    // 会静默把人格清空，用户以为改成功。
    if (!json_get_string(req.body, "persona", &text)) {
      Response r;
      r.status = 400;
      r.body = R"({"error":"missing_field","field":"persona"})";
      return r;
    }
    auto e = store_->set_default_persona(text);
    Response r;
    if (!e.ok()) {
      r.status = 500;
      r.body = R"({"error":"write_failed"})";
      return r;
    }
    r.body = R"({"ok":true})";
    return r;
  }

  // GET /v1/persona/session?conv_id=xxx
  if (req.method == "GET" && path == "/v1/persona/session") {
    const std::string conv = query_param(req.path, "conv_id");
    if (conv.empty()) {
      Response r;
      r.status = 400;
      r.body = R"({"error":"missing_conv_id"})";
      return r;
    }
    auto p = store_->persona_for(conv);
    Response r;
    if (!p.ok()) {
      r.status = 500;
      r.body = R"({"error":"read_failed"})";
      return r;
    }
    r.body = R"({"persona": ")" + json_escape(*p.value) + R"("})";
    return r;
  }

  // PUT /v1/persona/session?conv_id=xxx  {"persona":"..."}
  if (req.method == "PUT" && path == "/v1/persona/session") {
    const std::string conv = query_param(req.path, "conv_id");
    if (conv.empty()) {
      Response r;
      r.status = 400;
      r.body = R"({"error":"missing_conv_id"})";
      return r;
    }
    std::string text;
    if (!json_get_string(req.body, "persona", &text)) {
      Response r;
      r.status = 400;
      r.body = R"({"error":"missing_field","field":"persona"})";
      return r;
    }
    auto e = store_->set_persona_for(conv, text);
    Response r;
    if (!e.ok()) {
      r.status = 500;
      r.body = R"({"error":"write_failed"})";
      return r;
    }
    r.body = R"({"ok":true})";
    return r;
  }

  // DELETE /v1/persona/session?conv_id=xxx  清除覆盖
  if (req.method == "DELETE" && path == "/v1/persona/session") {
    const std::string conv = query_param(req.path, "conv_id");
    if (conv.empty()) {
      Response r;
      r.status = 400;
      r.body = R"({"error":"missing_conv_id"})";
      return r;
    }
    auto e = store_->set_persona_for(conv, "");
    Response r;
    if (!e.ok()) {
      r.status = 500;
      r.body = R"({"error":"write_failed"})";
      return r;
    }
    r.body = R"({"ok":true})";
    return r;
  }

  Response r;
  r.status = 404;
  r.body = R"({"error":"not_found"})";
  return r;
}

Response Server::dispatch(const Request& req) {
  // 探活免鉴权：外部监控进程通常拿不到 token，
  // 而 /healthz 不泄露任何信息（固定返回 ok）。
  // 放在鉴权检查之前，否则监控永远只能拿到 401。
  if (req.method == "GET" && path_only(req.path) == "/healthz") {
    const auto up = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - started_at_)
                        .count();
    Response r;
    r.body = R"({"ok":true,"version":")" + json_escape(cfg_.version) +
             R"(","uptime_s":)" + std::to_string(up) + "}";
    return r;
  }

  if (!check_auth(req)) {
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "api", "unauthorized")
        .field("path", path_only(req.path));
    Response r;
    r.status = 401;
    r.body = R"({"error":"unauthorized"})";
    return r;
  }

  // 先看注册的额外路由，再走内置
  {
    std::lock_guard<std::mutex> lock(routes_mu_);
    const auto it = routes_.find(to_lower(req.method) + " " + path_only(req.path));
    if (it != routes_.end()) return it->second(req);
  }
  return builtin_route(req);
}

void Server::handle_connection(int fd) {
  std::string raw;
  raw.reserve(4096);
  char buf[4096];

  // 控制面请求很小，1MB 已经很宽松。
  // 超限必须显式拒绝：早期是读到上限就停，然后照常解析，
  // 结果 2MB 的请求也返回 200，调用方以为写成功了。
  constexpr std::size_t kMaxBody = 1u << 20;

  // 读到 header 结束 + Content-Length 个 body 字节为止
  std::size_t want = std::string::npos;
  bool too_large = false;
  while (raw.size() < kMaxBody) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    raw.append(buf, static_cast<std::size_t>(n));

    if (want == std::string::npos) {
      const auto he = raw.find("\r\n\r\n");
      if (he != std::string::npos) {
        std::size_t body_len = 0;
        std::istringstream hs(raw.substr(0, he));
        std::string line;
        std::getline(hs, line);
        while (std::getline(hs, line)) {
          const auto colon = line.find(':');
          if (colon == std::string::npos) continue;
          if (to_lower(trim(line.substr(0, colon))) == "content-length") {
            try {
              body_len = static_cast<std::size_t>(std::stoul(trim(line.substr(colon + 1))));
            } catch (...) {
            }
          }
        }
        want = he + 4 + body_len;
        if (want > kMaxBody) {
          too_large = true;
          break;
        }
      }
    }
    if (want != std::string::npos && raw.size() >= want) break;
  }

  Request req;
  Response resp;
  if (too_large) {
    resp.status = 413;
    resp.body = R"({"error":"payload_too_large"})";
  } else if (!parse_request(raw, &req)) {
    resp.status = 400;
    resp.body = R"({"error":"bad_request"})";
  } else {
    resp = dispatch(req);
  }

  const std::string out = build_response(resp);
  std::size_t sent = 0;
  while (sent < out.size()) {
    const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) break;
    sent += static_cast<std::size_t>(n);
  }
}

}  // namespace fish::api
