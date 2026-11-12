// tests/test_transport.cpp
// 通道层测试：OneBot 事件解析、被动语义、能力声明。
#include <cstdio>
#include <string>

#include "fish/transport/local_channel.hpp"
#include "fish/transport/qq_onebot.hpp"

namespace {

int failures = 0;
#define CHECK(cond, msg)                 \
  do {                                   \
    if (!(cond)) {                       \
      std::printf("[FAIL] %s\n", msg);   \
      ++failures;                        \
    } else {                             \
      std::printf("[PASS] %s\n", msg);   \
    }                                    \
  } while (0)

void test_onebot_group_at_parsed() {
  fish::transport::OneBotConfig cfg;
  cfg.self_id = "10086";
  fish::transport::QQOneBotChannel ch(cfg);

  const std::string raw = R"({
    "post_type":"message",
    "message_type":"group",
    "group_id":123456,
    "user_id":999,
    "message_id":555,
    "sender":{"nickname":"张三"},
    "message":[
      {"type":"at","data":{"qq":"10086"}},
      {"type":"text","data":{"text":" 在吗"}}
    ]
  })";

  auto r = ch.parse_event(raw);
  CHECK(r.ok(), "group message parsed");
  if (r.ok()) {
    CHECK(r.value->kind == fish::core::ChatKind::kGroup, "kind is group");
    CHECK(r.value->mentioned_me, "at self_id sets mentioned_me");
    CHECK(r.value->conversation_id == "g:123456", "conversation id prefixed");
    CHECK(r.value->message_id == "555", "message id parsed");
    CHECK(r.value->sender_name == "张三", "sender name parsed");
    CHECK(!r.value->blocks.empty(), "has content blocks");
  }
}

void test_onebot_group_no_at() {
  fish::transport::OneBotConfig cfg;
  cfg.self_id = "10086";
  fish::transport::QQOneBotChannel ch(cfg);

  const std::string raw = R"({
    "post_type":"message","message_type":"group","group_id":1,
    "user_id":2,"message_id":3,"sender":{"nickname":"A"},
    "message":[{"type":"text","data":{"text":"随便聊聊"}}]
  })";

  auto r = ch.parse_event(raw);
  CHECK(r.ok(), "non-at group message parsed");
  CHECK(r.ok() && !r.value->mentioned_me, "no at means not mentioned");
}

void test_onebot_at_other_not_mention() {
  fish::transport::OneBotConfig cfg;
  cfg.self_id = "10086";
  fish::transport::QQOneBotChannel ch(cfg);

  const std::string raw = R"({
    "post_type":"message","message_type":"group","group_id":1,
    "user_id":2,"message_id":3,"sender":{"nickname":"A"},
    "message":[
      {"type":"at","data":{"qq":"99999"}},
      {"type":"text","data":{"text":" 你说啥"}}
    ]
  })";

  auto r = ch.parse_event(raw);
  CHECK(r.ok() && !r.value->mentioned_me, "at someone else is not a mention");
}

void test_onebot_private() {
  fish::transport::OneBotConfig cfg;
  cfg.self_id = "10086";
  fish::transport::QQOneBotChannel ch(cfg);

  const std::string raw = R"({
    "post_type":"message","message_type":"private","user_id":777,
    "message_id":888,"sender":{"nickname":"B"},
    "message":[{"type":"text","data":{"text":"你好"}}]
  })";

  auto r = ch.parse_event(raw);
  CHECK(r.ok(), "private message parsed");
  CHECK(r.ok() && r.value->kind == fish::core::ChatKind::kPrivate, "kind is private");
  CHECK(r.ok() && r.value->conversation_id == "777", "private conv id is user id");
}

void test_onebot_non_message_event() {
  fish::transport::OneBotConfig cfg;
  fish::transport::QQOneBotChannel ch(cfg);
  auto r = ch.parse_event(R"({"post_type":"notice"})");
  CHECK(!r.ok(), "non-message event rejected");
}

void test_onebot_bad_json() {
  fish::transport::OneBotConfig cfg;
  fish::transport::QQOneBotChannel ch(cfg);
  auto r = ch.parse_event("{not json");
  CHECK(!r.ok(), "bad json rejected");
}

void test_capability_declarations() {
  fish::transport::OneBotConfig oc;
  fish::transport::QQOneBotChannel onebot(oc);
  const auto cap = onebot.capability();
  CHECK(cap.supports_group, "onebot supports group");
  CHECK(cap.supports_voice_out, "onebot supports voice out");
  CHECK(cap.max_replies_per_message > 5, "onebot has loose reply budget");

  fish::transport::LocalChannel local;
  const auto lcap = local.capability();
  CHECK(lcap.supports_private, "local supports private");
}

void test_local_channel_roundtrip() {
  fish::transport::LocalChannel ch;
  ch.inject_text("c1", "hi", false, true);
  auto msg = ch.poll();
  CHECK(msg.ok(), "local poll returns injected message");
  CHECK(msg.ok() && (*msg.value).blocks.size() == 1, "one block");

  fish::core::OutboundMessage out;
  out.conversation_id = "c1";
  fish::core::ContentBlock b;
  b.kind = fish::core::BlockKind::kText;
  b.text = "reply";
  out.blocks.push_back(b);
  CHECK(ch.send(out, 1).ok(), "local send ok");
  CHECK(ch.sent_count() == 1, "sent recorded");
}

}  // namespace

int main() {
  std::printf("=== transport tests ===\n");
  test_onebot_group_at_parsed();
  test_onebot_group_no_at();
  test_onebot_at_other_not_mention();
  test_onebot_private();
  test_onebot_non_message_event();
  test_onebot_bad_json();
  test_capability_declarations();
  test_local_channel_roundtrip();

  if (failures != 0) {
    std::printf("\n%d FAILURES\n", failures);
    return 1;
  }
  std::printf("\nAll transport tests passed.\n");
  return 0;
}
