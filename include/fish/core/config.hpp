// include/fish/core/config.hpp
// 极简 TOML 子集解析器 + 环境变量展开。
// 不引入 toml++ 全量实现，只需支持本项目用到的语法。
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "fish/core/types.hpp"

namespace fish::core {

class Config {
 public:
  static Result<Config> load_from_file(const std::string& path);

  // 点号路径取值：cfg.get("capability.chat.model")
  [[nodiscard]] std::optional<std::string> get(const std::string& path) const;
  [[nodiscard]] std::string get_or(const std::string& path, const std::string& def) const;
  [[nodiscard]] long long get_int_or(const std::string& path, long long def) const;
  [[nodiscard]] double get_double_or(const std::string& path, double def) const;
  [[nodiscard]] bool get_bool_or(const std::string& path, bool def) const;
  [[nodiscard]] bool has(const std::string& path) const;

  // 收集所有缺失的必填项，一次性报出
  [[nodiscard]] std::vector<std::string> missing_required(
      const std::vector<std::string>& required) const;

  // 供测试与运行时构造
  void set(std::string path, std::string value) { kv_[std::move(path)] = std::move(value); }

 private:
  std::map<std::string, std::string> kv_;
};

// 展开 ${VAR} 与 ${VAR:-default}。未定义且无默认值时返回空并记录缺失。
std::string expand_env(const std::string& raw, std::vector<std::string>* missing_vars = nullptr);

}  // namespace fish::core
