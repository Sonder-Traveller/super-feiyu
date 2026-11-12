// src/fish.config.cppm
module;
#include <algorithm>
#include <cstdlib>
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

 private:
  std::map<std::string, std::string> kv_;
};

std::string expand_env(const std::string& raw, std::vector<std::string>* missing = nullptr);

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

}  // namespace

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
      const char* val = std::getenv(name.c_str());
      if (val != nullptr && *val != '\0') {
        out += val;
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

  Config cfg;
  std::string line;
  std::string section;
  std::vector<std::string> missing_env;

  while (std::getline(in, line)) {
    line = trim(strip_comment(line));
    if (line.empty()) continue;

    if (line.front() == '[' && line.back() == ']') {
      section = trim(line.substr(1, line.size() - 2));
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;

    std::string key = trim(line.substr(0, eq));
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
  }

  for (auto& [k, v] : cfg.kv_) {
    if (v.find("${") != std::string::npos) v = expand_env(v, &missing_env);
  }
  for (const auto& name : missing_env) {
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "config", "env_var_missing")
        .field("var", name);
  }

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

void Config::set(std::string path, std::string value) { kv_[std::move(path)] = std::move(value); }

}  // namespace fish::config
