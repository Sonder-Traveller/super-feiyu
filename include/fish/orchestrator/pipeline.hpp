// include/fish/orchestrator/pipeline.hpp
// 编排层：唯一持有业务决策。
#pragma once

#include <memory>
#include <string>

#include "fish/core/interfaces.hpp"
#include "fish/core/types.hpp"
#include "fish/memory/memory_system.hpp"

namespace fish::orchestrator {

class ReplyPolicy {
 public:
  struct Decision {
    bool should_reply{false};
    std::string reason;
  };

  [[nodiscard]] Decision evaluate(const core::InboundMessage& msg,
                                  const core::ChannelCapability& cap) const;
};

// 去重：平台会重发事件（重连、轮询超时），必须去重否则重复回复
class DedupCache {
 public:
  explicit DedupCache(std::size_t capacity = 1024);
  // 见过返回 true（并刷新时间）；首次见到返回 false
  bool seen(const std::string& key);
  std::size_t sweep();

 private:
  struct Entry {
    core::TimePoint at{};
  };
  std::size_t capacity_;
  std::mutex mu_;
  std::unordered_map<std::string, Entry> seen_;
};

class Pipeline {
 public:
  struct Deps {
    core::IChatModel* chat{nullptr};
    core::IEmbeddingModel* embedding{nullptr};
    core::IPersona* persona{nullptr};
    core::IReplyBudget* budget{nullptr};
    core::ITextToSpeech* tts{nullptr};
    core::ISpeechToText* stt{nullptr};
    memory::MemorySystem* memory{nullptr};
    memory::Consolidator* consolidator{nullptr};
    bool group_voice{false};
    int passive_window_ms{300000};  // 群聊默认 5 分钟
  };

  explicit Pipeline(Deps deps);

  // 返回实际发出的条数；0 表示按策略不回
  core::Result<int> handle(core::IChannel& channel, const core::InboundMessage& msg);

  DedupCache& dedup() { return dedup_; }

 private:
  core::Result<std::vector<core::ContentBlock>> normalize_input(
      const core::InboundMessage& msg);

  Deps deps_;
  ReplyPolicy policy_;
  DedupCache dedup_;
};

}  // namespace fish::orchestrator
