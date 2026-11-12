// include/fish/orchestrator/pipeline.hpp
// 编排层：唯一持有业务决策（要不要回、回什么、怎么回）。
// 它只依赖 core 的抽象，不知道 QQ 协议、也不知道 DeepSeek 的 HTTP 细节。
#pragma once

#include <memory>
#include <string>

#include "fish/core/interfaces.hpp"
#include "fish/core/types.hpp"

namespace fish::orchestrator {

// 被动对话的准入判定。独立成策略对象，便于单测和替换。
class ReplyPolicy {
 public:
  struct Decision {
    bool should_reply{false};
    std::string reason;  // 便于日志排查，比如 "group_not_mentioned"
  };

  [[nodiscard]] Decision evaluate(const core::InboundMessage& msg,
                                  const core::ChannelCapability& cap) const;
};

// 单条消息的完整处理链路。
// channel -> policy -> memory.recall -> persona -> chat -> budget -> channel
class Pipeline {
 public:
  struct Deps {
    core::IChatModel* chat{nullptr};
    core::IEmbeddingModel* embedding{nullptr};
    core::IMemoryStore* memory{nullptr};
    core::IPersona* persona{nullptr};
    core::IReplyBudget* budget{nullptr};
    core::ITextToSpeech* tts{nullptr};      // 可为空：不支持语音就不挂
    core::ISpeechToText* stt{nullptr};      // 可为空
  };

  explicit Pipeline(Deps deps);

  // 处理一轮。返回实际发出的消息条数（0 表示按策略不回）。
  core::Result<int> handle(core::IChannel& channel, const core::InboundMessage& msg);

 private:
  // 把入站多模态块转成「本轮用户输入块」：语音先转文字，图片原样透传。
  core::Result<std::vector<core::ContentBlock>> normalize_input(
      const core::InboundMessage& msg);

  // 组装历史上下文（短期窗口 + 语义召回），渲染成扁平块。
  core::Result<std::vector<core::ContentBlock>> build_history(const core::InboundMessage& msg,
                                                              const std::string& query);

  Deps deps_;
  ReplyPolicy policy_;
};

}  // namespace fish::orchestrator
