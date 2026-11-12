// tests/test_protocol.cpp
// 协议层测试：QQ 网关 op 码处理、OneBot 事件解析、WS 状态机。
// 全部离线，不联网。
#include <cstdio>
#include <string>
#include <vector>

import fish.core;
import fish.log;
import fish.ws;
import fish.qq_ws;
import fish.onebot_ws;

namespace {

int g_fail = 0;
#define CHECK(cond, msg)                    \
  do {                                      \
    if (!(cond)) {                          \
      std::printf("[FAIL] %s\n", msg);      \
      ++g_fail;                             \
    } else {                                \
      std::printf("[PASS] %s\n", msg);      \
    }                                       \
  } while (0)

// ── OneBot 事件解析（覆盖各类消息段）────────────────────
void test_onebot_segments() {
  std::printf("\n=== onebot segments ===\n");
  fish::onebot_ws::Config cfg;
  cfg.self_id = "10086";
  fish::onebot_ws::Client c(cfg);

  // 文本 + @ + 图片 + 语音 混合
  const std::string raw = R"({
    "post_type":"message","message_type":"group","group_id":42,
    "user_id":7,"message_id":99,"time":1700000000,
    "sender":{"card":"老王","nickname":"wang"},
    "message":[
      {"type":"at","data":{"qq":"10086"}},
      {"type":"text","data":{"text":" 看图"}},
      {"type":"image","data":{"url":"https://cdn/x.png"}},
      {"type":"record","data":{"url":"https://cdn/y.silk"}}
    ]
  })";
  auto r = c.parse_event(raw);
  CHECK(r.ok(), "mixed message parsed");
  if (r.ok()) {
    CHECK(r.value->segments.size() == 3, "3 content segments kept (at filtered)");
    CHECK(r.value->mentioned, "at self");
    CHECK(r.value->nickname == "老王", "card preferred over nickname");
    CHECK(r.value->group_id == "42", "group id");
    CHECK(r.value->time == 1700000000LL, "timestamp");
  }

  // 只有 at，无文本 → 应被判定为空消息
  const std::string only_at = R"({
    "post_type":"message","message_type":"group","group_id":1,
    "user_id":2,"message_id":3,
    "message":[{"type":"at","data":{"qq":"10086"}}]
  })";
  auto r2 = c.parse_event(only_at);
  CHECK(!r2.ok(), "at-only message rejected as empty");

  // @全体成员（all）不应触发 mentioned
  const std::string at_all = R"({
    "post_type":"message","message_type":"group","group_id":1,
    "user_id":2,"message_id":3,
    "message":[{"type":"at","data":{"qq":"all"}},{"type":"text","data":{"text":" hi"}}]
  })";
  auto r3 = c.parse_event(at_all);
  CHECK(r3.ok() && !r3.value->mentioned, "@all is not a mention of bot");

  // reply 段
  const std::string with_reply = R"({
    "post_type":"message","message_type":"group","group_id":1,
    "user_id":2,"message_id":3,
    "message":[{"type":"reply","data":{"id":"888"}},{"type":"text","data":{"text":"x"}}]
  })";
  auto r4 = c.parse_event(with_reply);
  CHECK(r4.ok(), "reply segment parsed");
}

// ── QQ 网关 op 码处理 ───────────────────────────────────
void test_qq_op_codes() {
  std::printf("\n=== qq gateway ops ===\n");
  fish::qq_ws::Config cfg;
  cfg.app_id = "test";
  fish::qq_ws::Client c(cfg);

  // op 10 HELLO：记录心跳间隔（不抛异常即可）
  c.handle_raw_frame(R"({"op":10,"d":{"heartbeat_interval":41250}})");
  CHECK(true, "op10 hello handled");

  // op 11 心跳 ACK
  c.handle_raw_frame(R"({"op":11})");
  CHECK(true, "op11 heartbeat ack handled");

  // op 0 READY：建立 session
  c.handle_raw_frame(R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-abc"}})");
  CHECK(c.session_id() == "sess-abc", "READY sets session_id");

  // op 0 群消息事件
  c.handle_raw_frame(R"({
    "op":0,"s":2,"t":"GROUP_AT_MESSAGE_CREATE",
    "d":{"id":"msg-1","group_openid":"grp-9",
         "author":{"id":"u1","username":"张三"},
         "content":"<@!12345> 你好"}
  })");
  CHECK(c.event_count() >= 1, "group event counted");

  // op 0 私聊事件
  c.handle_raw_frame(R"({
    "op":0,"s":3,"t":"C2C_MESSAGE_CREATE",
    "d":{"id":"msg-2","author":{"user_openid":"u2"},"content":"私聊一下"}
  })");
  CHECK(c.event_count() >= 2, "c2c event counted");

  // op 9 INVALID_SESSION：清 session
  c.handle_raw_frame(R"({"op":9,"d":false})");
  CHECK(c.session_id().empty(), "op9 clears session");

  // 畸形 JSON 不应崩溃
  c.handle_raw_frame("{not json");
  c.handle_raw_frame("");
  c.handle_raw_frame(R"({"op":999})");
  CHECK(true, "malformed frames survive");
}

// ── QQ 事件内容清洗 ─────────────────────────────────────
void test_qq_content_cleanup() {
  std::printf("\n=== qq content cleanup ===\n");
  fish::qq_ws::Config cfg;
  cfg.app_id = "t";
  fish::qq_ws::Client c(cfg);

  std::string captured;
  c.on_event([&](const fish::qq_ws::Event& ev) {
    if (ev.type == "GROUP_AT_MESSAGE_CREATE") captured = ev.content;
  });

  c.handle_raw_frame(R"({
    "op":0,"s":1,"t":"GROUP_AT_MESSAGE_CREATE",
    "d":{"id":"m1","group_openid":"g1","author":{"id":"u1"},
         "content":"<@!bot123> 说点啥"}
  })");
  CHECK(captured == "说点啥", "mention prefix stripped");

  captured.clear();
  c.handle_raw_frame(R"({
    "op":0,"s":2,"t":"GROUP_AT_MESSAGE_CREATE",
    "d":{"id":"m2","group_openid":"g1","author":{"id":"u1"},
         "content":"无前缀消息"}
  })");
  CHECK(captured == "无前缀消息", "plain content kept");
}

// ── WS 状态机 ───────────────────────────────────────────
void test_ws_state() {
  std::printf("\n=== ws state machine ===\n");
  fish::ws::Config cfg;
  cfg.url = "wss://invalid.invalid/ws";
  cfg.connect_timeout_ms = 3000;

  fish::ws::Client c(cfg);
  CHECK(c.state() == fish::ws::State::kIdle, "initial state idle");
  CHECK(!c.is_open(), "not open initially");

  auto e = c.connect();
  CHECK(!e.ok(), "connect to invalid host fails");
  CHECK(c.state() == fish::ws::State::kFailed, "state becomes failed");

  // 未连接时发送应报错而非崩溃
  auto s = c.send_text("hello");
  CHECK(!s.ok(), "send on closed socket fails gracefully");

  c.close();
  c.close();  // 幂等
  CHECK(true, "close is idempotent");
}

}  // namespace

int main() {
  fish::log::Logger::instance().set_level(fish::log::LogLevel::kError);

  test_onebot_segments();
  test_qq_op_codes();
  test_qq_content_cleanup();
  test_ws_state();

  std::printf("\n");
  if (g_fail != 0) {
    std::printf("%d FAILURES\n", g_fail);
    return 1;
  }
  std::printf("All protocol tests passed.\n");
  return 0;
}

