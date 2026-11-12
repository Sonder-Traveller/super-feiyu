// include/fish/core/reply_budget.hpp
#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

#include "fish/core/interfaces.hpp"

namespace fish::core {

// 被动回复预算的默认实现：按 message_id 记次数，带窗口过期回收。
// 做成独立类而不是内联在通道里，是因为 QQ/微信/未来通道的规则可以复用。
class ReplyBudget final : public IReplyBudget {
 public:
  explicit ReplyBudget(int limit_per_message,
                       std::chrono::seconds window = std::chrono::minutes(70));

  [[nodiscard]] int remaining(const std::string& message_id) const override;
  bool consume(const std::string& message_id) override;

  // 清理过期条目，避免长期运行内存只增不减。
  std::size_t sweep();

 private:
  struct Entry {
    int used{0};
    TimePoint expires_at{};
  };

  int limit_;
  std::chrono::seconds window_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace fish::core
