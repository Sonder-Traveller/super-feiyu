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

// 按 token 预算裁剪：超出预算的原文不进上下文，但会落盘
void test_token_budget_compaction() {
  std::printf("\n=== token 预算压缩 ===\n");
  const std::string base = "/tmp/fish_budget_" + std::to_string(std::rand());
  StubEmbedding emb;

  fish::memory::MemorySystem::Config c;
  c.notes_dir = base + "/notes";
  c.db_path = base + "/i.db";
  // 故意把窗口设很小，逼出压缩
  c.model_context_tokens = 200;
  c.raw_context_ratio = 0.5;   // raw 预算 = 100 token
  c.summary_ratio = 0.5;
  fish::memory::MemorySystem ms(c, &emb);
  CHECK(ms.init().ok(), "init ok");

  // 灌入足够多的对话，使总量超过 100 token
  for (int i = 0; i < 20; ++i) {
    fish::memory::Message u, a;
    u.role = "user";
    u.text = "这是第 " + std::to_string(i) + " 条比较长的消息内容内容内容";
    u.at = fish::core::Clock::now();
    a.role = "assistant";
    a.text = "好的我记住了第 " + std::to_string(i) + " 条";
    a.at = fish::core::Clock::now();
    ms.record("conv", u, a);
  }

  CHECK(ms.needs_compaction("conv"), "总量超预算 → 需要压缩");

  auto ctx_before = ms.gather("conv");
  CHECK(ctx_before.ok(), "gather ok");
  const std::size_t before_tokens = ctx_before.ok() ? ctx_before.value->estimated_tokens : 0;
  CHECK(before_tokens <= 120, "上下文被裁到预算内（<=120 token）");

  // 压缩（需要 consolidator；这里传一个假的 summarize 模型）
  SlowChat chat;
  fish::memory::Consolidator cons(&chat, &ms.notes(), &ms.index(), &emb);
  auto rep = ms.compact("conv", cons);
  CHECK(rep.ok(), "compact 成功");

  auto ctx_after = ms.gather("conv");
  CHECK(ctx_after.ok(), "压缩后 gather ok");
  CHECK(ctx_after.ok() && !ctx_after.value->summary.empty(), "生成了摘要");

  // 原文没丢：应能从索引/旁路文件检索到
  auto hits = ms.search("conv", "第 15 条", 5);
  CHECK(hits.ok() && !hits.value->empty(), "压缩掉的内容仍可检索到");

  fs::remove_all(base);
}

// 未超预算时 gather 全量返回
void test_gather_full_when_under_budget() {
  std::printf("\n=== 未超预算时全量 ===\n");
  const std::string base = "/tmp/fish_budget2_" + std::to_string(std::rand());
  StubEmbedding emb;

  fish::memory::MemorySystem::Config c;
  c.notes_dir = base + "/notes";
  c.db_path = base + "/i.db";
  c.model_context_tokens = 131072;   // 大窗口
  fish::memory::MemorySystem ms(c, &emb);
  ms.init();

  for (int i = 0; i < 50; ++i) {
    fish::memory::Message u, a;
    u.role = "user";
    u.text = "短消息 " + std::to_string(i);
    u.at = fish::core::Clock::now();
    a.role = "assistant";
    a.text = "收到 " + std::to_string(i);
    a.at = fish::core::Clock::now();
    ms.record("c2", u, a);
  }

  CHECK(!ms.needs_compaction("c2"), "远低于预算 → 不需要压缩");
  auto ctx = ms.gather("c2");
  CHECK(ctx.ok(), "gather ok");
  CHECK(ctx.ok() && ctx.value->working.size() == 100, "50 轮 = 100 条全部返回（一份不少）");

  fs::remove_all(base);
}

// 原文缓冲的硬上限：无论配置写多高，最多占模型窗口的 50%
void test_raw_buffer_cap() {
  std::printf("\n=== 原文缓冲 50% 上限 ===\n");
  const int window = 100000;
  const std::size_t half = static_cast<std::size_t>(window) / 2;

  // 配置想占 90%，实际被压回 50%
  auto greedy = fish::memory::TokenBudget::make(window, 0.90, 0.25);
  CHECK(greedy.raw_budget == half, "配置 0.90 → 仍压到窗口的 50%");

  // 配置低于上限时按配置走，不被抬高
  auto modest = fish::memory::TokenBudget::make(window, 0.30, 0.25);
  CHECK(modest.raw_budget == 30000, "配置 0.30 → 按 30% 走");

  // 不合法 / 未配置时取默认值（0.40），不是取上限
  auto fallback = fish::memory::TokenBudget::make(window, 0.0, 0.25);
  CHECK(fallback.raw_budget == 40000, "未配置 → 默认 40%，不因有上限就抬到 50%");

  // 恰好等于上限时不误伤
  auto exact = fish::memory::TokenBudget::make(window, 0.50, 0.25);
  CHECK(exact.raw_budget == half, "配置正好 0.50 → 原样保留");

  // 下限保护：配得过低（每轮都压缩、疯狂调 LLM）→ 抬到下限
  const std::size_t min_budget =
      static_cast<std::size_t>(window * fish::memory::TokenBudget::kRawBufferMinRatio);
  auto too_small = fish::memory::TokenBudget::make(window, 0.01, 0.25);
  CHECK(too_small.raw_budget == min_budget, "配置 0.01 → 抬到下限 5%");

  // 三段预算加起来不超过窗口
  CHECK(modest.raw_budget + modest.summary_budget + modest.reserve ==
            static_cast<std::size_t>(window),
        "原文 + 摘要 + 预留 = 窗口，不超发");
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
  test_token_budget_compaction();
  test_gather_full_when_under_budget();
  test_raw_buffer_cap();
  test_pipeline_maintenance();

  std::printf("\n");
  if (g_fail != 0) {
    std::printf("%d FAILURES\n", g_fail);
    return 1;
  }
  std::printf("All lifecycle tests passed.\n");
  return 0;
}
