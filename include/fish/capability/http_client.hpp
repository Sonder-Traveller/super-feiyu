// include/fish/capability/http_client.hpp
// 基于 libcurl 的同步 HTTP 客户端：重试、超时、熔断。
#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "fish/core/types.hpp"

namespace fish::capability {

struct HttpRequest {
  std::string method{"POST"};
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  int timeout_ms{30000};
  int max_retries{2};
  // 二进制响应（下载音频）时为 true，结果放在 bytes
  bool want_binary{false};
};

struct HttpResult {
  int status{0};
  std::string body;
  std::string bytes;
  std::map<std::string, std::string> headers;
};

// 按 provider 名独立熔断，避免一家挂掉拖累另一家
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
  core::TimePoint open_until_{};
};

class HttpClient {
 public:
  static HttpClient& instance();

  // 全局初始化/清理（在 main 里调用一次）
  static void global_init();
  static void global_cleanup();

  core::Result<HttpResult> perform(const HttpRequest& req, CircuitBreaker* breaker = nullptr);

  // 便捷方法
  core::Result<HttpResult> post_json(const std::string& url,
                                     const std::map<std::string, std::string>& headers,
                                     const std::string& json_body, int timeout_ms = 30000,
                                     CircuitBreaker* breaker = nullptr);

  core::Result<std::string> download(const std::string& url, std::size_t max_bytes = 16u << 20);

 private:
  HttpClient() = default;
};

}  // namespace fish::capability
