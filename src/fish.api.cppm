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
  };

  using Handler = std::function<Response(const Request&)>;

  Server(Config cfg, fish::persona::PersonaStore* store);
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

 private:
  void accept_loop();
  void handle_connection(int fd);
  Response dispatch(const Request& req);
  Response builtin_route(const Request& req);
  bool check_auth(const Request& req) const;

  Config cfg_;
  fish::persona::PersonaStore* store_{nullptr};
  std::map<std::string, Handler> routes_;   // "GET /x" → handler
  std::mutex routes_mu_;

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
// 上完整 JSON 解析器不值当。value 支持常规转义。
bool json_get_string(const std::string& json, const std::string& key, std::string* out) {
  const std::string needle = "\"" + key + "\"";
  auto pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos = json.find('"', pos);
  if (pos == std::string::npos) return false;

  std::string val;
  for (std::size_t i = pos + 1; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '\\' && i + 1 < json.size()) {
      const char n = json[++i];
      switch (n) {
        case 'n': val += '\n'; break;
        case 'r': val += '\r'; break;
        case 't': val += '\t'; break;
        case '"': val += '"'; break;
        case '\\': val += '\\'; break;
        case '/': val += '/'; break;
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

Server::Server(Config cfg, fish::persona::PersonaStore* store)
    : cfg_(std::move(cfg)), store_(store) {}

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

  // /healthz 在 dispatch 里已处理（免鉴权），这里不再重复。

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
    json_get_string(req.body, "persona", &text);
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
    json_get_string(req.body, "persona", &text);
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
    Response r;
    r.body = R"({"ok":true})";
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

  // 读到 header 结束 + Content-Length 个 body 字节为止
  std::size_t want = std::string::npos;
  while (raw.size() < (1u << 20)) {   // 1MB 上限，控制面用不到这么大
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
      }
    }
    if (want != std::string::npos && raw.size() >= want) break;
  }

  Request req;
  Response resp;
  if (!parse_request(raw, &req)) {
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
