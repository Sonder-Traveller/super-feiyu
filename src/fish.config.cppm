// src/fish.config.cppm
module;
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

export module fish.config;

import fish.core;
import fish.log;

export namespace fish::config {

// 极简 TOML 子集 + ${VAR} 展开。只支持本项目用到的语法。
class Config {
 public:
  static fish::core::Result<Config> load(const std::string& path);

  [[nodiscard]] std::optional<std::string> get(const std::string& path) const;
  [[nodiscard]] std::string get_or(const std::string& path, const std::string& def) const;
  [[nodiscard]] long long get_int_or(const std::string& path, long long def) const;
  [[nodiscard]] double get_double_or(const std::string& path, double def) const;
  [[nodiscard]] bool get_bool_or(const std::string& path, bool def) const;
  [[nodiscard]] bool has(const std::string& path) const;
  [[nodiscard]] std::vector<std::string> missing_required(
      const std::vector<std::string>& required) const;
  void set(std::string path, std::string value);

  // 展开**之前**的原始值。
  //
  // 为什么需要：${VAR} 展开后就是一把真 key，从值本身看不出它绑的是
  // 哪个厂商。判断「这把 key 属于 siliconflow 还是 deepseek」必须看
  // 原始写法（见 capability 的 resolve_provider_key）。
  [[nodiscard]] std::optional<std::string> get_raw(const std::string& path) const;

  // 该键是否写成 ${VAR} 形式。是则返回变量名。
  //
  // 只认「整个值就是一个 ${VAR}」的写法（与控制面 ConfigFile::
  // env_var_of 口径一致）：这才是「密钥占位符」；像 "Bearer ${X}"
  // 这种是普通配置，不算绑定。
  [[nodiscard]] std::optional<std::string> env_var_of(const std::string& path) const;

  // 配置文件所在目录下的 .env.local 路径（Config::load 时确定）。
  [[nodiscard]] const std::string& env_file_path() const noexcept { return env_file_path_; }

 private:
  std::map<std::string, std::string> kv_;
  std::map<std::string, std::string> raw_;
  std::string env_file_path_;
};

std::string expand_env(const std::string& raw, std::vector<std::string>* missing = nullptr);

// 查一个环境变量：先查进程环境，再查 config.toml 同目录的 .env.local。
//
// 为什么要统一口径：控制面（fish.control）读密钥是「.env.local → 进程环境」，
// 运行时以前只查进程环境。GUI 在启动后写 .env.local（set_secret 写文件，
// 不改进程环境）时，控制面显示 key_ready=true，运行时却拿到空串 →
// 服务起不来。两者必须同一个口径。
[[nodiscard]] std::optional<std::string> lookup_env(const std::string& name);

}  // namespace fish::config

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::config {

namespace {

std::string trim(std::string s) {
  const auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r'; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string strip_comment(const std::string& s) {
  bool in_str = false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '"') in_str = !in_str;
    if (s[i] == '#' && !in_str) return s.substr(0, i);
  }
  return s;
}

std::string unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

// .env.local 的解析结果（一次性读，按需查）。
//
// 口径与控制面 fish.control::EnvFile 保持一致：
//   - 跳过空行与 # 注释
//   - 按第一个 '=' 分割，值原样保留（不做引号剥离）
//   - 首个匹配优先（后面的同名行不覆盖前面的）
class EnvFileCache {
 public:
  EnvFileCache() = default;

  void load(const std::string& path) {
    kv_.clear();
    if (path.empty()) return;
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string t = trim(line);
      if (t.empty() || t.front() == '#') continue;
      const auto eq = t.find('=');
      if (eq == std::string::npos) continue;
      std::string k = trim(t.substr(0, eq));
      if (k.empty()) continue;
      std::string v = t.substr(eq + 1);
      // 值两端空白剥掉（Windows 的 CRLF / 行尾空格会让鉴权失败）
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' ||
                            v.back() == '\n')) {
        v.pop_back();
      }
      if (kv_.find(k) == kv_.end()) kv_.emplace(std::move(k), std::move(v));
    }
  }

  [[nodiscard]] std::optional<std::string> get(const std::string& name) const {
    auto it = kv_.find(name);
    if (it == kv_.end()) return std::nullopt;
    if (it->second.empty()) return std::nullopt;
    return it->second;
  }

 private:
  std::map<std::string, std::string> kv_;
};

// 进程级状态：.env.local 的路径与缓存。
//
// 路径由 Config::load() 确定（配置文件同目录），之后所有 ${VAR} 展开
// 共用。用函数局部 static 而不是全局变量，避免模块初始化顺序问题。
std::string& env_file_path_slot() {
  static std::string path;
  return path;
}

EnvFileCache& env_cache_slot() {
  static EnvFileCache cache;
  return cache;
}

}  // namespace

std::optional<std::string> lookup_env(const std::string& name) {
  // 1. 进程环境（systemd 有 EnvironmentFile=.env.local 时两者一致）
  if (const char* val = std::getenv(name.c_str()); val != nullptr && *val != '\0') {
    return std::string{val};
  }
  // 2. 配置文件同目录的 .env.local（GUI 启动后写入、进程环境未更新时靠这条）
  const std::string& path = env_file_path_slot();
  if (path.empty()) return std::nullopt;
  return env_cache_slot().get(name);
}

std::string expand_env(const std::string& raw, std::vector<std::string>* missing) {
  std::string out;
  std::size_t i = 0;
  while (i < raw.size()) {
    if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
      const auto end = raw.find('}', i + 2);
      if (end == std::string::npos) {
        out += raw.substr(i);
        break;
      }
      std::string expr = raw.substr(i + 2, end - i - 2);
      std::string name = expr;
      std::string def;
      bool has_def = false;
      const auto sep = expr.find(":-");
      if (sep != std::string::npos) {
        name = expr.substr(0, sep);
        def = expr.substr(sep + 2);
        has_def = true;
      }
      const auto found = lookup_env(name);
      if (found.has_value() && !found->empty()) {
        // 剥掉尾部 CR/LF：Windows 写的 .env 经 shell source 后会带 \r，
        // 直接拼进密钥会导致鉴权失败（100007 appid invalid）。
        std::string sv = *found;
        while (!sv.empty() &&
               (sv.back() == '\r' || sv.back() == '\n' || sv.back() == ' ' ||
                sv.back() == '\t')) {
          sv.pop_back();
        }
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
          sv.erase(sv.begin());
        }
        out += sv;
      } else if (has_def) {
        out += def;
      } else if (missing != nullptr) {
        missing->push_back(name);
      }
      i = end + 1;
    } else {
      out += raw[i];
      ++i;
    }
  }
  return out;
}

fish::core::Result<Config> Config::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return fish::core::Result<Config>::fail(fish::core::ErrorCode::kBadRequest,
                                            "cannot_open_config:" + path);
  }

  // .env.local 与 config.toml 同目录（部署脚本保证这一点）。
  // 先把路径定下来并读一次，之后所有 ${VAR} 展开都能回退到它。
  {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::path(path).parent_path();
    const std::string env_path = (base.empty() ? std::filesystem::path(".env.local")
                                                : (base / ".env.local"))
                                     .string();
    env_file_path_slot() = env_path;
    env_cache_slot().load(env_path);
  }

  Config cfg;
  std::string line;
  std::string section;
  std::size_t line_no = 0;
  std::vector<std::string> missing_env;

  while (std::getline(in, line)) {
    ++line_no;
    line = trim(strip_comment(line));
    if (line.empty()) continue;

    // 段头必须以 ] 结尾。`[broken` 这种未闭合的段头如果被静默跳过，
    // 后面所有键都会落进上一个 section，用户看到的报错会完全无关
    // （例如只提示「api_key 为空」），排查成本极高。
    if (line.front() == '[') {
      if (line.back() != ']') {
        return fish::core::Result<Config>::fail(
            fish::core::ErrorCode::kBadRequest,
            "config_parse_error:line " + std::to_string(line_no) +
                ": unclosed section header: " + line);
      }
      section = trim(line.substr(1, line.size() - 2));
      continue;
    }

    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      // TOML 里裸词或未闭合的引号都会走到这里。跳过等于让用户的笔误
      // 变成「配置莫名其妙没生效」，必须显式报错。
      return fish::core::Result<Config>::fail(
          fish::core::ErrorCode::kBadRequest,
          "config_parse_error:line " + std::to_string(line_no) +
              ": expected key = value, got: " + line);
    }

    std::string key = trim(line.substr(0, eq));
    if (key.empty()) {
      return fish::core::Result<Config>::fail(
          fish::core::ErrorCode::kBadRequest,
          "config_parse_error:line " + std::to_string(line_no) + ": empty key");
    }

    std::string val = trim(line.substr(eq + 1));

    if (!val.empty() && val.front() == '[') {
      const auto close = val.find(']');
      if (close != std::string::npos) val = val.substr(1, close - 1);
      std::string joined;
      std::stringstream ss(val);
      std::string item;
      while (std::getline(ss, item, ',')) {
        item = unquote(trim(item));
        if (item.empty()) continue;
        if (!joined.empty()) joined += ",";
        joined += item;
      }
      val = joined;
    } else {
      val = unquote(val);
    }

    const std::string full = section.empty() ? key : (section + "." + key);
    cfg.kv_[full] = val;
    // 保留展开前的原值：判断「这把 key 绑的是哪个厂商」要看写法本身
    // （${SILICONFLOW_API_KEY} vs 明文），展开后就看不出来了。
    cfg.raw_[full] = val;
  }

  for (auto& [k, v] : cfg.kv_) {
    if (v.find("${") != std::string::npos) v = expand_env(v, &missing_env);
  }
  for (const auto& name : missing_env) {
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "config", "env_var_missing")
        .field("var", name);
  }

  cfg.env_file_path_ = env_file_path_slot();

  return fish::core::Result<Config>::ok(std::move(cfg));
}

std::optional<std::string> Config::get(const std::string& path) const {
  auto it = kv_.find(path);
  if (it == kv_.end()) return std::nullopt;
  return it->second;
}

std::string Config::get_or(const std::string& path, const std::string& def) const {
  auto v = get(path);
  if (!v || v->empty()) return def;
  return *v;
}

long long Config::get_int_or(const std::string& path, long long def) const {
  auto v = get(path);
  if (!v || v->empty()) return def;
  try {
    return std::stoll(*v);
  } catch (...) {
    return def;
  }
}

double Config::get_double_or(const std::string& path, double def) const {
  auto v = get(path);
  if (!v || v->empty()) return def;
  try {
    return std::stod(*v);
  } catch (...) {
    return def;
  }
}

bool Config::get_bool_or(const std::string& path, bool def) const {
  auto v = get(path);
  if (!v || v->empty()) return def;
  const std::string& s = *v;
  return s == "true" || s == "1" || s == "yes" || s == "on";
}

bool Config::has(const std::string& path) const { return kv_.find(path) != kv_.end(); }

std::vector<std::string> Config::missing_required(
    const std::vector<std::string>& required) const {
  std::vector<std::string> missing;
  for (const auto& path : required) {
    auto v = get(path);
    if (!v || v->empty()) missing.push_back(path);
  }
  return missing;
}

void Config::set(std::string path, std::string value) {
  kv_[path] = value;
  raw_[path] = value;
}

std::optional<std::string> Config::get_raw(const std::string& path) const {
  auto it = raw_.find(path);
  if (it == raw_.end()) return std::nullopt;
  return it->second;
}

std::optional<std::string> Config::env_var_of(const std::string& path) const {
  const auto raw = get_raw(path);
  if (!raw.has_value()) return std::nullopt;
  const std::string& s = *raw;
  // 只认「整个值就是一个 ${VAR}」：这才是密钥占位符。
  // "Bearer ${X}" 这种是普通配置，不是绑定。
  if (s.size() > 4 && s[0] == '$' && s[1] == '{' && s.back() == '}') {
    return s.substr(2, s.size() - 3);
  }
  return std::nullopt;
}

}  // namespace fish::config
