// src/layered_persona.cpp
#include "fish/persona/layered_persona.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

namespace fish::persona {

using core::ErrorCode;
using core::Result;

LayeredPersona::LayeredPersona(std::string persona_text, std::string constraints) {
  // 前缀构造只做一次，且顺序固定：约束在前、人格在后。
  // 顺序一旦变化，所有历史缓存立刻失效，所以这里写死。
  std::ostringstream oss;
  oss << constraints << "\n\n" << persona_text;
  prefix_ = oss.str();
}

std::string LayeredPersona::stable_prefix() const { return prefix_; }

std::string LayeredPersona::dynamic_suffix(const core::InboundMessage& msg,
                                           const std::string& recalled_memory) const {
  std::ostringstream oss;

  // 时间只精确到小时，减少尾部抖动；且绝不放回前缀。
  const auto now = core::Clock::to_time_t(msg.received_at);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &now);
#else
  localtime_r(&now, &tm_buf);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H时", &tm_buf);

  oss << "【本轮上下文】\n";
  oss << "现在时间：" << buf << "\n";
  oss << "场景：" << (msg.kind == core::ChatKind::kGroup ? "QQ群聊" : "私聊") << "\n";
  oss << "对方：" << msg.sender_name << "\n";

  if (!recalled_memory.empty()) {
    oss << "\n【相关记忆】\n" << recalled_memory << "\n";
  }

  // 被动模式的硬约束放在最尾部，属于"近因强化"位置，执行力最强。
  oss << "\n【本轮红线】只回这一条，不要主动开启新话题；"
      << "不超过两句；不要暴露你是 AI。";

  return oss.str();
}

Result<LayeredPersona> load_persona(const std::string& persona_path,
                                    const std::string& constraints_path) {
  auto read_file = [](const std::string& path) -> Result<std::string> {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return Result<std::string>::fail(ErrorCode::kBadRequest, "cannot_open:" + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    auto text = ss.str();
    if (text.empty()) {
      return Result<std::string>::fail(ErrorCode::kBadRequest, "empty_file:" + path);
    }
    return Result<std::string>::ok(std::move(text));
  };

  auto persona = read_file(persona_path);
  if (!persona.ok()) return Result<LayeredPersona>::fail(persona.error.code, persona.error.detail);

  auto constraints = read_file(constraints_path);
  if (!constraints.ok()) {
    return Result<LayeredPersona>::fail(constraints.error.code, constraints.error.detail);
  }

  return Result<LayeredPersona>::ok(
      LayeredPersona(std::move(*persona.value), std::move(*constraints.value)));
}

}  // namespace fish::persona
