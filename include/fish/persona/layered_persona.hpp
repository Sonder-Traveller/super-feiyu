// include/fish/persona/layered_persona.hpp
#pragma once

#include <string>

#include "fish/core/interfaces.hpp"

namespace fish::persona {

// 分层人格：把提示词拆成"逐字节固定"的前缀和"每轮变化"的尾部。
//
// 为什么必须分层：DeepSeek 前缀缓存命中价 0.02 元/百万，未命中 1 元/百万。
// 差 50 倍。任何把时间戳、随机数、召回记忆拼进前缀的写法都会废掉缓存。
class LayeredPersona final : public core::IPersona {
 public:
  // persona_text: 纯人格描述（不含任何动态内容）
  // constraints: 稳定行为约束（被动模式红线），也会进前缀
  LayeredPersona(std::string persona_text, std::string constraints);

  [[nodiscard]] std::string stable_prefix() const override;

  [[nodiscard]] std::string dynamic_suffix(const core::InboundMessage& msg,
                                           const std::string& recalled_memory) const override;

 private:
  std::string prefix_;  // 构造时一次性拼好，之后只读
};

// 从文件加载人格。文件缺失时必须显式失败，不能静默用空人格。
core::Result<LayeredPersona> load_persona(const std::string& persona_path,
                                          const std::string& constraints_path);

}  // namespace fish::persona
