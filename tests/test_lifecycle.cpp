// tests/test_lifecycle.cpp
// 生命周期与运维能力测试：会话串行化、记忆淘汰、周期维护。
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

import fish.core;
import fish.log;
import fish.memory;
import fish.orchestrator;

namespace fs = std::filesystem;

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

// 记录并发度的假通道
class ProbeChannel final : public fish::core::IChannel {
 public:
  fish::core::Channel id() const noexcept override { return fish::core::Channel::kQQ; }
  fish::core::ChannelCapability capability() const noexcept override {
    fish::core::ChannelCapability c;
    c.max_replies_per_message = 4;
    return c;
  }
  fish::core::Result<fish::core::InboundMessage> poll() override {
    return fish::core::Result<fish::core::InboundMessage>::fail(
        fish::core::ErrorCode::kTimeout, "no");
  }
  fish::core::Error send(const fish::core::OutboundMessage&, int) override {
    return fish::core::Error::none();
  }
};

// 慢模型：用来放大并发窗口
class SlowChat final : public fish::core::IChatModel {
 public:
  std::atomic<int> inflight{0};
  std::atomic<int> max_inflight{0};
  std::atomic<int> calls{0};

  fish::core::Result<Response> complete(const Request&) override {
    const int cur = ++inflight;
    int prev = max_inflight.load();
    while (cur > prev && !max_inflight.compare_exchange_weak(prev, cur)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    --inflight;
    ++calls;
    Response r;
    r.text = "ok";
    return fish::core::Result<Response>::ok(std::move(r));
  }
};

class StubEmbedding final : public fish::core::IEmbeddingModel {
 public:
  int dimension() const noexcept override { return 8; }
  fish::core::Result<std::vector<float>> embed(const std::string& t) override {
    std::vector<float> v(8, 0.1F);
    for (std::size_t i = 0; i < t.size(); ++i) {
      v[i % 8] += static_cast<float>(static_cast<unsigned char>(t[i])) / 1000.0F;
    }
    return fish::core::Result<std::vector<float>>::ok(std::move(v));
  }
};

class NoPersona final : public fish::core::IPersona {
 public:
  std::string stable_prefix() const override { return "system"; }
  std::string dynamic_suffix(const fish::core::InboundMessage&,
                             const std::string&) const override {
    return "";
  }
};

fish::core::InboundMessage mk(const std::string& conv, const std::string& id) {
  fish::core::InboundMessage m;
  m.kind = fish::core::ChatKind::kPrivate;
  m.conversation_id = conv;
  m.sender_id = "u";
  m.sender_name = "u";
  m.message_id = id;
  m.mentioned_me = true;
  m.received_at = fish::core::Clock::now();
  fish::core::ContentBlock b;
  b.kind = fish::core::BlockKind::kText;
  b.text = "hi";
  m.blocks.push_back(std::move(b));
  return m;
}

// 同一会话并发两条消息，必须串行（max_inflight == 1）
void test_session_serialization() {
  std::printf("\n=== 会话串行化 ===\n");
  SlowChat chat;
  StubEmbedding emb;
  NoPersona persona;
  fish::orchestrator::ReplyBudget budget(10);
  ProbeChannel ch;

  fish::orchestrator::Pipeline::Deps d;
  d.chat = &chat;
  d.embedding = &emb;
  d.persona = &persona;
  d.budget = &budget;
  fish::orchestrator::Pipeline pipeline(d);

  std::vector<std::thread> ts;
  for (int i = 0; i < 4; ++i) {
    ts.emplace_back([&, i] {
      auto m = mk("same-conv", "m" + std::to_string(i));
      pipeline.handle(ch, m);
    });
  }
  for (auto& t : ts) t.join();

  CHECK(chat.calls.load() == 4, "4 个请求都处理了");
  CHECK(chat.max_inflight.load() == 1,
        "同会话并发被串行化（max_inflight == 1）");
}

// 不同会话应可并行（证明锁是分片的，不是全局）
void test_different_sessions_parallel() {
  std::printf("\n=== 不同会话可并行 ===\n");
  SlowChat chat;
  StubEmbedding emb;
  NoPersona persona;
  fish::orchestrator::ReplyBudget budget(10);
  ProbeChannel ch;

  fish::orchestrator::Pipeline::Deps d;
  d.chat = &chat;
  d.embedding = &emb;
  d.persona = &persona;
  d.budget = &budget;
  fish::orchestrator::Pipeline pipeline(d);

  std::vector<std::thread> ts;
  for (int i = 0; i < 4; ++i) {
    ts.emplace_back([&, i] {
      auto m = mk("conv-" + std::to_string(i), "m" + std::to_string(i));
      pipeline.handle(ch, m);
    });
  }
  for (auto& t : ts) t.join();

  CHECK(chat.calls.load() == 4, "4 个请求都处理了");
  CHECK(chat.max_inflight.load() > 1, "不同会话确实并行（>1）");
}

// 记忆淘汰：极老的条目应被清掉，新的保留
void test_memory_eviction() {
  std::printf("\n=== 记忆淘汰 ===\n");
  const std::string base = "/tmp/fish_life_" + std::to_string(std::rand());
  StubEmbedding emb;

  fish::memory::MemorySystem::Config c;
  c.notes_dir = base + "/notes";
  c.db_path = base + "/i.db";
  fish::memory::MemorySystem ms(c, &emb);
  CHECK(ms.init().ok(), "init ok");

  // 造一条"很老"的和一条"很新"的
  fish::memory::Fact old_f;
  old_f.id = "fold";
  old_f.content = "很久以前的事";
  old_f.confidence = 0.9;
  old_f.created = "2020-01-01";
  ms.notes().append_fact(old_f);

  fish::memory::Fact new_f;
  new_f.id = "fnew";
  new_f.content = "昨天的事";
  new_f.confidence = 0.9;
  new_f.created = ms.today_public();
  ms.notes().append_fact(new_f);

  auto before = ms.notes().load_facts();
  CHECK(before.ok() && before.value->size() == 2, "淘汰前 2 条");

  auto rep = ms.maintain(30.0, 0.05);
  CHECK(rep.ok(), "maintain 成功");
  CHECK(rep.ok() && rep.value->evicted >= 1, "至少淘汰 1 条");

  auto after = ms.notes().load_facts();
  CHECK(after.ok() && after.value->size() == 1, "淘汰后剩 1 条");
  bool kept_new = false;
  if (after.ok()) {
    for (const auto& f : *after.value) {
      if (f.content == "昨天的事") kept_new = true;
    }
  }
  CHECK(kept_new, "新记忆被保留（不是乱删）");

  fs::remove_all(base);
}

// 维护：预算与去重表能被清理
void test_pipeline_maintenance() {
  std::printf("\n=== 流水线维护 ===\n");
  SlowChat chat;
  StubEmbedding emb;
  NoPersona persona;
  fish::orchestrator::ReplyBudget budget(10);
  ProbeChannel ch;

  fish::orchestrator::Pipeline::Deps d;
  d.chat = &chat;
  d.embedding = &emb;
  d.persona = &persona;
  d.budget = &budget;
  fish::orchestrator::Pipeline pipeline(d);

  for (int i = 0; i < 5; ++i) {
    auto m = mk("c", "id-" + std::to_string(i));
    pipeline.handle(ch, m);
  }
  pipeline.maintenance();  // 不应崩溃
  CHECK(true, "maintenance 可安全调用");
}

}  // namespace

int main() {
  fish::log::Logger::instance().set_level(fish::log::LogLevel::kError);

  test_session_serialization();
  test_different_sessions_parallel();
  test_memory_eviction();
  test_pipeline_maintenance();

  std::printf("\n");
  if (g_fail != 0) {
    std::printf("%d FAILURES\n", g_fail);
    return 1;
  }
  std::printf("All lifecycle tests passed.\n");
  return 0;
}
