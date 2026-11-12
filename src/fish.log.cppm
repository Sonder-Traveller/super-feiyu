module;
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

export module fish.log;

import fish.core;

export namespace fish::log {

enum class LogLevel : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

class Logger {
 public:
  static Logger& instance();

  void set_level(LogLevel lv) noexcept;
  void set_json(bool on) noexcept;
  [[nodiscard]] LogLevel level() const noexcept;

  class Line {
   public:
    Line(Logger& owner, LogLevel lv, std::string_view module, std::string_view event);
    ~Line();
    Line& field(std::string_view k, std::string_view v);
    Line& field(std::string_view k, long long v);
    Line& field(std::string_view k, unsigned long long v);
    Line& field(std::string_view k, int v);
    Line& field(std::string_view k, double v, int precision);
    Line& field(std::string_view k, double v);
    Line& field(std::string_view k, bool v);

   private:
    void comma();
    Logger& owner_;
    LogLevel lv_;
    std::string buf_;
    bool first_{true};
  };

  void write(std::string_view line);

 private:
  Logger() = default;
  [[nodiscard]] static const char* level_name(LogLevel lv) noexcept;
  [[nodiscard]] static std::string escape(std::string_view s);

  LogLevel level_{LogLevel::kInfo};
  bool json_{true};
  std::mutex mu_;
};

// 模块无法导出宏，提供函数版入口（配合 Logger::Line 流式 field）
inline Logger::Line log_line(LogLevel lv, std::string_view m, std::string_view e) {
  return Logger::Line(Logger::instance(), lv, m, e);
}

}  // namespace fish::log

#define FISH_LOG(lv, module, event) \
  ::fish::log::Logger::Line(::fish::log::Logger::instance(), lv, module, event)
#define LOG_DEBUG(module, event) FISH_LOG(::fish::log::LogLevel::kDebug, module, event)
#define LOG_INFO(module, event) FISH_LOG(::fish::log::LogLevel::kInfo, module, event)
#define LOG_WARN(module, event) FISH_LOG(::fish::log::LogLevel::kWarn, module, event)
#define LOG_ERROR(module, event) FISH_LOG(::fish::log::LogLevel::kError, module, event)

// ── 实现 ────────────────────────────────────────────────
module :private;

namespace fish::log {

Logger& Logger::instance() {
  static Logger inst;
  return inst;
}

void Logger::set_level(LogLevel lv) noexcept { level_ = lv; }
void Logger::set_json(bool on) noexcept { json_ = on; }
LogLevel Logger::level() const noexcept { return level_; }

const char* Logger::level_name(LogLevel lv) noexcept {
  switch (lv) {
    case LogLevel::kDebug: return "debug";
    case LogLevel::kInfo: return "info";
    case LogLevel::kWarn: return "warn";
    case LogLevel::kError: return "error";
  }
  return "info";
}

std::string Logger::escape(std::string_view s) {
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

Logger::Line::Line(Logger& owner, LogLevel lv, std::string_view module,
                   std::string_view event)
    : owner_(owner), lv_(lv) {
  if (static_cast<int>(lv_) < static_cast<int>(owner_.level_)) {
    buf_.clear();
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);

  char ts[40];
  const long off = tm_buf.tm_gmtoff;
  const int off_h = static_cast<int>(off / 3600);
  const int off_m = static_cast<int>((off < 0 ? -off : off) % 3600) / 60;
  std::snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d.%03d%+03d:%02d",
                tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour,
                tm_buf.tm_min, tm_buf.tm_sec, static_cast<int>(ms.count()), off_h, off_m);

  buf_ += "{\"ts\":\"";
  buf_ += ts;
  buf_ += "\",\"level\":\"";
  buf_ += Logger::level_name(lv_);
  buf_ += "\",\"module\":\"";
  buf_ += escape(module);
  buf_ += "\",\"event\":\"";
  buf_ += escape(event);
  buf_ += "\"";
  first_ = false;
}

Logger::Line::~Line() {
  if (buf_.empty()) return;
  buf_ += "}";
  owner_.write(buf_);
}

void Logger::Line::comma() {
  if (!first_) buf_ += ",";
  first_ = false;
}

Logger::Line& Logger::Line::field(std::string_view k, std::string_view v) {
  if (buf_.empty()) return *this;
  comma();
  buf_ += "\"";
  buf_ += escape(k);
  buf_ += "\":\"";
  buf_ += escape(v);
  buf_ += "\"";
  return *this;
}

Logger::Line& Logger::Line::field(std::string_view k, long long v) {
  if (buf_.empty()) return *this;
  comma();
  buf_ += "\"";
  buf_ += escape(k);
  buf_ += "\":";
  buf_ += std::to_string(v);
  return *this;
}

Logger::Line& Logger::Line::field(std::string_view k, unsigned long long v) {
  if (buf_.empty()) return *this;
  comma();
  buf_ += "\"";
  buf_ += escape(k);
  buf_ += "\":";
  buf_ += std::to_string(v);
  return *this;
}

Logger::Line& Logger::Line::field(std::string_view k, int v) {
  return field(k, static_cast<long long>(v));
}

Logger::Line& Logger::Line::field(std::string_view k, double v, int precision) {
  if (buf_.empty()) return *this;
  comma();
  char num[64];
  std::snprintf(num, sizeof(num), "%.*f", precision, v);
  buf_ += "\"";
  buf_ += escape(k);
  buf_ += "\":";
  buf_ += num;
  return *this;
}

Logger::Line& Logger::Line::field(std::string_view k, double v) {
  return field(k, v, 4);
}

Logger::Line& Logger::Line::field(std::string_view k, bool v) {
  if (buf_.empty()) return *this;
  comma();
  buf_ += "\"";
  buf_ += escape(k);
  buf_ += "\":";
  buf_ += v ? "true" : "false";
  return *this;
}

void Logger::write(std::string_view line) {
  std::lock_guard<std::mutex> lock(mu_);
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

}  // namespace fish::log

