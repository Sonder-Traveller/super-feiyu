// tests/test_policy.cpp
// 被动对话的核心红线必须有测试保护：群聊未 @ 不得回复。
#include <cassert>
#include <iostream>

#include "fish/core/reply_budget.hpp"
#include "fish/orchestrator/pipeline.hpp"

using fish::core::ChannelCapability;
using fish::core::ChatKind;
using fish::core::InboundMessage;
using fish::orchestrator::ReplyPolicy;

namespace {

InboundMessage make_msg(ChatKind kind, bool mentioned) {
  InboundMessage m;
  m.kind = kind;
  m.mentioned_me = mentioned;
  m.conversation_id = "100";
  m.message_id = "m1";
  m.received_at = fish::core::Clock::now();
  return m;
}

void test_group_not_mentioned_is_silent() {
  ReplyPolicy p;
  ChannelCapability cap;
  auto d = p.evaluate(make_msg(ChatKind::kGroup, false), cap);
  assert(!d.should_reply);
  assert(d.reason == "group_not_mentioned");
  std::cout << "[PASS] group_not_mentioned_is_silent\n";
}

void test_group_mentioned_replies() {
  ReplyPolicy p;
  ChannelCapability cap;
  auto d = p.evaluate(make_msg(ChatKind::kGroup, true), cap);
  assert(d.should_reply);
  std::cout << "[PASS] group_mentioned_replies\n";
}

void test_private_always_replies() {
  ReplyPolicy p;
  ChannelCapability cap;
  auto d = p.evaluate(make_msg(ChatKind::kPrivate, false), cap);
  assert(d.should_reply);
  std::cout << "[PASS] private_always_replies\n";
}

void test_budget_enforces_limit() {
  fish::core::ReplyBudget budget(2);
  assert(budget.remaining("m1") == 2);
  assert(budget.consume("m1"));
  assert(budget.consume("m1"));
  assert(!budget.consume("m1"));  // 第三次必须失败
  assert(budget.remaining("m1") == 0);
  // 不同消息互不影响
  assert(budget.remaining("m2") == 2);
  std::cout << "[PASS] budget_enforces_limit\n";
}

void test_unsupported_channel_rejects() {
  ReplyPolicy p;
  ChannelCapability cap;
  cap.supports_private = false;
  auto d = p.evaluate(make_msg(ChatKind::kPrivate, false), cap);
  assert(!d.should_reply);
  std::cout << "[PASS] unsupported_channel_rejects\n";
}

}  // namespace

int main() {
  test_group_not_mentioned_is_silent();
  test_group_mentioned_replies();
  test_private_always_replies();
  test_budget_enforces_limit();
  test_unsupported_channel_rejects();
  std::cout << "\nAll tests passed.\n";
  return 0;
}
