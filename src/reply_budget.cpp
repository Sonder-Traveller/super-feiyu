// src/reply_budget.cpp
#include "fish/core/reply_budget.hpp"

namespace fish::core {

ReplyBudget::ReplyBudget(int limit_per_message, std::chrono::seconds window)
    : limit_(limit_per_message), window_(window) {}

int ReplyBudget::remaining(const std::string& message_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = entries_.find(message_id);
  if (it == entries_.end()) return limit_;

  const auto now = Clock::now();
  if (now >= it->second.expires_at) return limit_;  // 已过期，视作全新
  return limit_ - it->second.used;
}

bool ReplyBudget::consume(const std::string& message_id) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = Clock::now();

  auto it = entries_.find(message_id);
  if (it == entries_.end() || now >= it->second.expires_at) {
    entries_[message_id] = Entry{1, now + window_};
    return true;
  }
  if (it->second.used >= limit_) return false;

  ++it->second.used;
  return true;
}

std::size_t ReplyBudget::sweep() {
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = Clock::now();
  std::size_t removed = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (now >= it->second.expires_at) {
      it = entries_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

}  // namespace fish::core
