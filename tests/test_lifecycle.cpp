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

// 容量淘汰：超过配额时按权重保留，而不是单纯删最旧
void test_memory_capacity_eviction() {
  std::printf("\n=== 容量淘汰 ===\n");
  const std::string base = "/tmp/fish_life_" + std::to_string(std::rand());
  StubEmbedding emb;

  fish::memory::MemorySystem::Config c;
  c.notes_dir = base + "/notes";
  c.db_path = base + "/i.db";
  c.max_facts = 3;   // 配额 3
  c.max_episodes = 2;
  fish::memory::MemorySystem ms(c, &emb);
  CHECK(ms.init().ok(), "init ok");

  const std::string today = ms.today_public();

  // 造 6 条事实：其中 2 条老的、4 条新的
  auto add = [&](const std::string& id, const std::string& content, double conf,
                 const std::string& date) {
    fish::memory::Fact f;
    f.id = id;
    f.content = content;
    f.confidence = conf;
    f.created = date;
    ms.notes().append_fact(f);
  };
  add("old1", "很久以前随口说的", 0.3, "2020-01-01");
  add("old2", "很久以前随口说的2", 0.3, "2020-06-01");
  add("new1", "昨天说的重要事", 0.95, today);
  add("new2", "今天说的重要事", 0.95, today);
  add("new3", "刚才说的", 0.9, today);
  add("new4", "刚刚又说的", 0.9, today);

  auto before = ms.notes().load_facts();
  CHECK(before.ok() && before.value->size() == 6, "淘汰前 6 条");

  auto rep = ms.maintain(c.max_facts, c.max_episodes);
  CHECK(rep.ok(), "maintain 成功");
  CHECK(rep.ok() && rep.value->evicted_facts == 3, "淘汰 3 条（6 → 3）");

  auto after = ms.notes().load_facts();
  CHECK(after.ok() && after.value->size() == 3, "剩 3 条，等于配额");

  // 保留的应该都是新的（高权重）
  int new_kept = 0;
  if (after.ok()) {
    for (const auto& f : *after.value) {
      if (f.id.rfind("new", 0) == 0) ++new_kept;
    }
  }
  CHECK(new_kept == 3, "按权重保留（留新不留旧，不是随机删）");

  fs::remove_all(base);
}

// 未超配额时不动
void test_capacity_no_eviction_when_under() {
  std::printf("\n=== 未超配额不淘汰 ===\n");
  const std::string base = "/tmp/fish_life2_" + std::to_string(std::rand());
  StubEmbedding emb;

  fish::memory::MemorySystem::Config c;
  c.notes_dir = base + "/notes";
  c.db_path = base + "/i.db";
  fish::memory::MemorySystem ms(c, &emb);
  ms.init();

  for (int i = 0; i < 5; ++i) {
    fish::memory::Fact f;
    f.id = "f" + std::to_string(i);
    f.content = "记忆" + std::to_string(i);
    f.confidence = 0.9;
    f.created = ms.today_public();
    ms.notes().append_fact(f);
  }

  auto rep = ms.maintain(100, 100);  // 配额远大于实际
  CHECK(rep.ok(), "maintain 成功");
  CHECK(rep.ok() && rep.value->skipped, "未超配额 → 跳过");
  CHECK(rep.ok() && rep.value->evicted_facts == 0, "没删任何东西");

  auto after = ms.notes().load_facts();
  CHECK(after.ok() && after.value->size() == 5, "5 条原样保留");

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
  test_memory_capacity_eviction();
  test_capacity_no_eviction_when_under();
  test_pipeline_maintenance();

  std::printf("\n");
  if (g_fail != 0) {
    std::printf("%d FAILURES\n", g_fail);
    return 1;
  }
  std::printf("All lifecycle tests passed.\n");
  return 0;
}
