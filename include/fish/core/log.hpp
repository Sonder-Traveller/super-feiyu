// include/fish/core/log.hpp
// 结构化单行 JSON 日志。不引入 spdlog，需求简单且要控制二进制体积。
#pragma once

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

namespace fish::core {

enum class LogLevel : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

class Log {
 public:
  static Log& instance();

  void set_level(LogLevel lv) noexcept { level_ = lv; }
  void set_json(bool on) noexcept { json_ = on; }
  [[nodiscard]] LogLevel level() const noexcept { return level_; }

  // 简单 key=value 追加器，避免调用方手拼 JSON 出错
  class Line {
   public:
    Line(Log& owner, LogLevel lv, std::string_view module, std::string_view event);
    ~Line();

    Line& field(std::string_view k, std::string_view v);
    Line& field(std::string_view k, long long v);
    Line& field(std::string_view k, unsigned long long v);
    // int 单独重载：否则 int 同时可转 long long/double/bool，产生歧义
    Line& field(std::string_view k, int v) {
      return field(k, static_cast<long long>(v));
    }
    Line& field(std::string_view k, double v, int precision = 4);
    Line& field(std::string_view k, bool v);

   private:
    void comma();

    Log& owner_;
    LogLevel lv_;
    std::string buf_;
    bool first_{true};
  };

  void write_raw(std::string_view line);

 private:
  Log() = default;
  [[nodiscard]] static const char* level_name(LogLevel lv) noexcept;
  [[nodiscard]] static std::string json_escape(std::string_view s);

  LogLevel level_{LogLevel::kInfo};
  bool json_{true};
  std::mutex mu_;
};

// 流式用法：LOG_INFO("module", "event").field("k", v);
#define FISH_LOG(lv, module, event) \
  ::fish::core::Log::Line(::fish::core::Log::instance(), lv, module, event)

#define LOG_DEBUG(module, event) FISH_LOG(::fish::core::LogLevel::kDebug, module, event)
#define LOG_INFO(module, event) FISH_LOG(::fish::core::LogLevel::kInfo, module, event)
#define LOG_WARN(module, event) FISH_LOG(::fish::core::LogLevel::kWarn, module, event)
#define LOG_ERROR(module, event) FISH_LOG(::fish::core::LogLevel::kError, module, event)

}  // namespace fish::core
