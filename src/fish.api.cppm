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
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// control::json 是 fish.control 导出的 nlohmann 别名；这里显式 include
// 一次，保证本 TU 里能用它的成员（/v1/status 补字段、/v1/conversations）。
#include <nlohmann/json.hpp>

export module fish.api;

import fish.core;
import fish.log;
import fish.persona;
import fish.session;
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
  // 凭据端点专用令牌（清空 = 走控制面自动生成的那个）。
  //
  // 为什么单独一个：这个端点返回明文 access_token。主 token 是给 GUI
  // 用的（可能被展示、截图、贴进工单），拿它换明文凭据风险太大。
  // 独立令牌只认这一个端点，泄露了也只泄露 WorkBuddy 凭据。
  std::string credential_token;
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
  //
  // 回调产出的是一段 JSON 数组文本，每项至少含 name / received / errors。
  // 在线与否由回调自己算：api 层不知道「最近一次 poll 是成功还是失败」，
  // 也不该猜——早期这里对每项硬编码了 connected:true，看起来永远在线。
  // 现在：
  //   - 回调给了 connected 就原样透传（真实状态）；
  //   - 没给就按 received/errors 推断，并且保守返回 false 而不是 true；
  //   - 另外统一补一个 last_poll_ok（有 connected 就是它，没有就是 null），
  //     GUI 想区分「没连上」和「连上了但没消息」时有字段可用。
  void set_status_provider(std::function<std::string()> cb) {
    status_cb_ = std::move(cb);
  }

  // 注册重启回调。POST /v1/restart 时调用（异步线程里），
  // main 里实现成 raise(SIGTERM)，让 systemd Restart=always 接住拉起新进程。
  void set_restart_callback(std::function<void()> cb) { restart_cb_ = std::move(cb); }

  // 会话登记表：GET /v1/conversations 从这读。可为空（空时返回空列表）。
  void set_conversation_book(fish::session::ConversationBook* book) {
    convos_ = book;
  }

 private:
  void accept_loop();
  void handle_connection(int fd);
  Response dispatch(const Request& req);
  Response builtin_route(const Request& req);
  bool check_auth(const Request& req) const;

  // 该路径是否允许「凭据专用令牌」通过（白名单）。
  //
  // 白名单只有凭据端点一个：主 token 能干的事很多（改配置、扫码登录、
  // 改人格），凭据令牌只能读 WorkBuddy 凭据这一个动作。
  static bool credential_path_allowed(const std::string& path);

  // 绑定地址是不是回环。
  //
  // 只有回环时，「无鉴权模式下也放行凭据端点」才是可接受的：
  // 回环只有本机能连。非回环（0.0.0.0 等）时凭据端点必须带令牌。
  [[nodiscard]] bool loopback_bind() const;

  // 凭据专用令牌（懒加载 + 缓存）。
  //
  // 为什么要懒加载而不是在 main 里塞进 Config：凭据令牌要能从
  // config.toml 读、也要能在没配时自动生成到 data/ 下，这两件事都得
  // 控制面知道。api 层只负责问控制面要一次并缓存起来。
  [[nodiscard]] std::string credential_token() const;

  Config cfg_;
  fish::persona::PersonaStore* store_{nullptr};
  fish::control::ControlService* control_{nullptr};
  fish::session::ConversationBook* convos_{nullptr};
  std::map<std::string, Handler> routes_;   // "GET /x" → handler
  std::mutex routes_mu_;
  std::function<std::string()> status_cb_;
  std::function<void()> restart_cb_;
  // 凭据令牌缓存（mutable：check_auth 是 const 的）。
  mutable std::mutex cred_token_mu_;
  mutable std::string cred_token_cache_;
  mutable bool cred_token_loaded_{false};

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

// ── URL 查询串解码 ───────────────────────────────────────

// 十六进制字符 → 数值；不是十六进制返回 -1
int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// UTF-8 合法性校验：拒绝过长编码、代理区码点、越界码点。
bool valid_utf8(const std::string& s) {
  std::size_t i = 0;
  const std::size_t n = s.size();
  while (i < n) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t len = 0;
    unsigned int cp = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07u;
    } else {
      return false;  // 孤立的续字节，或 0xF8 以上
    }
    if (i + len > n) return false;
    for (std::size_t k = 1; k < len; ++k) {
      const auto cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (len == 2 && cp < 0x80) return false;      // 过长编码
    if (len == 3 && cp < 0x800) return false;
    if (len == 4 && cp < 0x10000) return false;
    if (cp > 0x10FFFF) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // 代理区
    i += len;
  }
  return true;
}

// percent-decode：先把 %XX 还原成字节，再整体按 UTF-8 解码。
//
// 为什么必须解码：GUI 用 urllib.parse.quote(x, safe="") 编码后发过来的是
// conv_id=qq%3A1906397285。服务端若按字面量切分，落盘的 key 就是
// "qq%3A1906397285"，而运行时读的是 "qq:1906397285" —— 两个不同的会话，
// 于是 GUI 保存的人格和会话人格机器人都读不到。GUI 自己 GET 同一个
// 编码 URL 能读回（自洽），所以界面上完全看不出问题。
//
// UTF-8 多字节（%E4%B8%AD 这种）不能一个 %XX 解一个字符：先还原成字节
// 序列，整体再按 UTF-8 解，顺序反了就是乱码。
//
// plus_as_space：
//   true  —— '+' 还原成空格（application/x-www-form-urlencoded 语义，
//           Python 的 urlencode / requests 就是用 '+' 表示空格）。
//   false —— '+' 保持字面量。给不透明 token 用：base64 token 里的 '+'
//           是内容的一部分，按空格解就再也对不上原值了。
//
// 解不出来（非法 % 转义、截断的转义、非法 UTF-8）就原样返回：
// 不抛异常、不 500 —— 一个畸形查询串不该把整个请求打死。
std::string percent_decode(const std::string& in, bool plus_as_space) {
  std::string bytes;
  bytes.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '%') {
      if (i + 2 >= in.size()) return in;   // 转义被截断
      const int hi = hex_val(in[i + 1]);
      const int lo = hex_val(in[i + 2]);
      if (hi < 0 || lo < 0) return in;     // 非法 %XX
      bytes += static_cast<char>((hi << 4) | lo);
      i += 2;
      continue;
    }
    if (c == '+' && plus_as_space) {
      bytes += ' ';
      continue;
    }
    bytes += c;
  }
  if (!valid_utf8(bytes)) return in;       // 不是合法 UTF-8 → 原样
  return bytes;
}

// URL 里取查询参数值（会做 percent-decode）。
//
// 解码规则见 percent_decode：conv_id 这类标识按 form 规范把 '+' 当空格，
// task_id / qrcode / login_id 这类不透明 token 传 false，'+' 保持字面量。
std::string query_param(const std::string& path, const std::string& key,
                        bool plus_as_space = true) {
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
      return percent_decode(piece.substr(eq + 1), plus_as_space);
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

  // 凭据端点返回**明文** access_token。绑到 0.0.0.0 意味着同网段任何
  // 人都能来取这个 token（只要有凭据令牌，而令牌就在本机的文件里）。
  //
  // 所以绑非回环地址时必须显式说「我知道风险」，否则这个端点就是一个
  // 半公开的凭据分发口。默认拒绝，用户确实需要（比如桥接层在另一台机器）
  // 再显式打开。
  if (!loopback_bind()) {
    const char* allow = std::getenv("FEIYU_ALLOW_REMOTE_CREDENTIAL");
    if (allow == nullptr || std::string(allow).empty() ||
        std::string(allow) == "0" || std::string(allow) == "false") {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "api",
                            "credential_endpoint_exposed")
          .field("bind", cfg_.bind_addr)
          .field("hint",
                 "凭据端点会返回明文 token。确认要暴露到非回环地址，"
                 "请设 FEIYU_ALLOW_REMOTE_CREDENTIAL=1");
    }
  }
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

bool Server::credential_path_allowed(const std::string& path) {
  // 只有这一个端点。刻意不放宽到「所有 /v1/capability/workbuddy/*」：
  // 登录 / 测试端点不需要凭据令牌。
  return path == "/v1/capability/workbuddy/credential";
}

bool Server::loopback_bind() const {
  const std::string& a = cfg_.bind_addr;
  return a == "127.0.0.1" || a == "localhost" || a == "::1";
}

std::string Server::credential_token() const {
  // 配置里显式给的优先（main 里可以传，也可以留空走控制面自动生成）
  if (!cfg_.auth.credential_token.empty()) return cfg_.auth.credential_token;
  if (control_ == nullptr) return {};

  {
    std::lock_guard<std::mutex> lock(cred_token_mu_);
    if (cred_token_loaded_) return cred_token_cache_;
  }
  // 控制面负责：读 config.toml 的 capability.workbuddy.credential_token，
  // 没配就生成并写到 data/control_credential_token.txt。
  const std::string tok = control_->workbuddy_credential_token();
  std::lock_guard<std::mutex> lock(cred_token_mu_);
  cred_token_cache_ = tok;
  cred_token_loaded_ = true;
  return cred_token_cache_;
}

bool Server::check_auth(const Request& req) const {
  const std::string path = path_only(req.path);
  const bool cred_path = credential_path_allowed(path);

  if (!cfg_.auth.enabled) {
    // 无鉴权模式（本机调试）：其他端点照常放行。
    //
    // 但凭据端点返回**明文** access_token，不能跟着一起裸奔：
    // 绑到非回环地址时直接拒绝，让用户改用「开启鉴权 + 凭据令牌」。
    if (cred_path && !loopback_bind()) return false;
    return true;
  }
  if (cfg_.auth.token.empty()) return false;

  const auto it = req.headers.find("authorization");
  if (it == req.headers.end()) return false;

  const std::string& v = it->second;
  const std::string prefix = "bearer ";
  if (v.size() <= prefix.size()) return false;
  if (to_lower(v.substr(0, prefix.size())) != prefix) return false;
  const std::string presented = v.substr(prefix.size());

  if (constant_time_equal(presented, cfg_.auth.token)) return true;

  // 凭据专用令牌：只对白名单路径生效。
  //
  // 顺序很重要：先比主 token（绝大多数请求走这条），主 token 不匹配才
  // 考虑凭据令牌，且凭据令牌只放行白名单里的那一个端点。
  if (cred_path) {
    const std::string ct = credential_token();
    if (!ct.empty() && constant_time_equal(presented, ct)) return true;
  }
  return false;
}

Response Server::builtin_route(const Request& req) {
  const std::string path = path_only(req.path);

  // ── 健康检查与版本 ──
  // /healthz 在 dispatch 里已处理（免鉴权），这里不再重复。
  if (req.method == "GET" && path == "/v1/status") {
    const auto up = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - started_at_)
                        .count();
    // 通道状态：回调给什么就是什么，但这里补两点：
    //   1) connected 缺省时不再假装在线（原来是硬编码 true）；
    //   2) 统一补 last_poll_ok，让调用方能区分「通道没连上」和
    //      「连上了只是最近没消息」。
    // 回调文本解析不了时原样透传，不影响 /v1/status 本身可用。
    std::string channels = status_cb_ ? status_cb_() : "[]";
    try {
      auto arr = control::json::parse(channels);
      if (arr.is_array()) {
        for (auto& item : arr) {
          if (!item.is_object()) continue;
          if (!item.contains("connected")) item["connected"] = false;
          if (!item.contains("last_poll_ok")) {
            // 回调没说「上次 poll 成没成」就明确给 null，不要拿 connected
            // 顶替——main 里 connected 目前还是硬编码 true，照抄等于
            // 把假信号伪装成真状态。
            item["last_poll_ok"] = nullptr;
          }
        }
        channels = arr.dump();
      }
    } catch (const std::exception&) {
      // 回调给的不是合法 JSON：保留原样，至少不 500
    }
    std::string body = R"({"version":")" + json_escape(cfg_.version) +
                       R"(","uptime_s":)" + std::to_string(up) + R"(,"channels":)";
    body += channels;
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
      // task_id 是上游给的透明 token，'+' 可能就是内容的一部分 → 不当空格解
      auto j = control_->qq_qr_status(query_param(req.path, "task_id", false));
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
      // qrcode 同样是上游给的透明 token → '+' 不当空格解
      auto j = control_->wechat_qr_status(query_param(req.path, "qrcode", false));
      Response r;
      if (!j.ok()) {
        r.status = 502;
        r.body = R"({"status":"error","detail":")" + json_escape(j.error.detail) + R"("})";
        return r;
      }
      r.body = *j.value;
      return r;
    }
    // ── WorkBuddy 链接登录 ──
    // 不用填 Key：拿一个授权链接，用户在浏览器里登录，我们再查结果。
    if (req.method == "POST" && path == "/v1/capability/workbuddy/login") {
      auto j = control_->workbuddy_login_start();
      Response r;
      if (!j.ok()) {
        r.status = 502;
        r.body = R"({"ok":false,"error":"login_start_failed","detail":")" +
                 json_escape(j.error.detail) + R"("})";
        return r;
      }
      r.body = *j.value;
      return r;
    }

    if (req.method == "GET" && path == "/v1/capability/workbuddy/login/status") {
      // login_id 是本进程生成的 "wb_" + 16 位 hex，只可能是 [0-9a-f_]，
      // 但仍按不透明 token 处理（'+' 不当空格），将来改生成方式也不会中招
      auto j = control_->workbuddy_login_status(query_param(req.path, "login_id", false));
      Response r;
      if (!j.ok()) {
        r.status = 502;
        r.body = R"({"ok":false,"error":"status_failed","detail":")" +
                 json_escape(j.error.detail) + R"("})";
        return r;
      }
      r.body = *j.value;
      return r;
    }

    if (req.method == "POST" && path == "/v1/capability/workbuddy/test") {
      auto j = control_->workbuddy_test();
      Response r;
      if (!j.ok()) {
        r.status = 502;
        r.body = R"({"ok":false,"error":"test_failed","detail":")" +
                 json_escape(j.error.detail) + R"("})";
        return r;
      }
      r.body = *j.value;
      return r;
    }

    // ── WorkBuddy 凭据（跨平台桥接层拉取）──
    //
    // 这个端点返回**明文** access_token，所以它只认「凭据专用令牌」
    // （见 check_auth / AuthConfig::credential_token），主 token 也行，
    // 但桥接层应该用专用令牌。
    //
    // 为什么需要它：控制面在 WSL、桥接层在 Windows 宿主时，两边看到的
    // 不是同一个路径（`\\wsl$\...` vs `C:\...`），--cred-file 永远对不上，
    // GUI 扫码登录对桥就是零效果。走 HTTP 与文件系统视图无关。
    if (req.method == "GET" && path == "/v1/capability/workbuddy/credential") {
      auto c = control_->workbuddy_credential();
      Response r;
      if (!c.found) {
        r.status = 404;
        r.body = R"({"error":"no_credential","detail":")" +
                 json_escape(c.detail) + R"("})";
        return r;
      }
      r.body = c.body;
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

  // GET /v1/conversations —— 会话登记表（人格页 bot 卡片列表）
  if (req.method == "GET" && path == "/v1/conversations") {
    Response r;
    control::json arr = control::json::array();
    if (convos_ != nullptr) {
      for (const auto& e : convos_->list()) {
        arr.push_back({{"key", e.key},
                       {"channel", e.channel},
                       {"kind", e.kind},
                       {"title", e.title},
                       {"sender_name", e.sender_name},
                       {"sender_id", e.sender_id},
                       {"last_active_ms", e.last_active_ms},
                       {"handled", e.handled}});
      }
    }
    r.body = control::json{{"conversations", arr}}.dump();
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
