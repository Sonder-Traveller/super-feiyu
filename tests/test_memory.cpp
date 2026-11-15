// tests/test_memory.cpp
// 三层记忆的核心行为测试。
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

// 模块化之后不再有头文件，直接用 import。
// 这个文件曾长期不在 build.sh 的构建列表里，所以 include 一直停留在
// 头文件时代（fish/core/interfaces.hpp 早就不存在了）也没人发现。
import fish.core;
import fish.log;
import fish.memory;

namespace fs = std::filesystem;

namespace {

// 确定性桩 embedding，避免测试打网络
class StubEmbedding final : public fish::core::IEmbeddingModel {
 public:
  int dimension() const noexcept override { return 8; }
  fish::core::Result<std::vector<float>> embed(const std::string& text) override {
    std::vector<float> v(8, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      v[i % 8] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    for (auto& x : v) x += 0.01F;  // 避免零向量
    return fish::core::Result<std::vector<float>>::ok(std::move(v));
  }
};

int failures = 0;

// 会话隔离改造后，几乎所有 NotesStore / Consolidator 接口都要求 conv_id。
// 测试统一用这一个会话键，避免每处手写不一致。
constexpr const char* CONV = "mem-test-conv";
#define CHECK(cond, msg)                            \
  do {                                              \
    if (!(cond)) {                                  \
      std::printf("[FAIL] %s\n", msg);              \
      ++failures;                                   \
    } else {                                        \
      std::printf("[PASS] %s\n", msg);              \
    }                                               \
  } while (0)

void test_working_memory_capacity() {
  fish::memory::WorkingMemory wm(2);  // 只保留 2 轮
  for (int i = 0; i < 5; ++i) {
    fish::memory::Message u, a;
    u.role = "user";
    u.text = "u" + std::to_string(i);
    u.at = fish::core::Clock::now();
    a.role = "assistant";
    a.text = "a" + std::to_string(i);
    a.at = fish::core::Clock::now();
    wm.push(u, a);
  }
  CHECK(wm.round_count() == 2, "working memory keeps only max_rounds");
  auto snap = wm.snapshot();
  CHECK(snap.size() == 4, "snapshot has 2 rounds = 4 messages");
  CHECK(snap.front().text == "u3", "oldest evicted, keeps most recent");
}

// 记忆只增不减：没有衰减、没有淘汰。
//
// 这两个函数（test_decay / test_eviction_selection）原本测的是
// fish::memory::Forgetting —— 一个按半衰期衰减、按权重淘汰的实现。
// 用户明确要求「记忆只增不减、不论时间只论数量」，那段代码已删除，
// 测试却留了下来（而且没被构建，所以一直没人发现编译不过）。
//
// 现在改为验证「写入多条后全部仍在」，这才是当前设计的正确行为。
void test_no_eviction() {
  const auto dir = fs::temp_directory_path() / "fish_noevict_test";
  fs::remove_all(dir);
  fs::create_directories(dir);

  fish::memory::NotesStore notes(dir.string());
  const std::string conv = "noevict-conv";
  CHECK(notes.ensure_dirs().ok(), "notes init");

  constexpr int kCount = 50;
  bool all_ok = true;
  for (int i = 0; i < kCount; ++i) {
    fish::memory::Fact f;
    f.id = "f" + std::to_string(i);
    f.content = "content-" + std::to_string(i);
    f.confidence = 0.01;   // 故意给最低置信度
    f.created = "2020-01-01";   // 故意给最早日期
    if (!notes.append_fact(conv, f).ok()) all_ok = false;
  }
  CHECK(all_ok, "50 条低置信度旧事实全部写入成功");

  // 关键断言：50 条全在，一条都没被时间或置信度淘汰
  auto loaded = notes.load_facts(conv);
  CHECK(loaded.ok(), "load facts");
  CHECK(loaded.ok() && loaded.value->size() == kCount,
        "no fact evicted by age or confidence");

  fs::remove_all(dir);
}

void test_notes_roundtrip() {
  const std::string dir = "/tmp/fish_test_notes_" + std::to_string(std::rand());
  fish::memory::NotesStore notes(dir);
  notes.ensure_dirs();

  // 会话隔离改造后，所有 NotesStore 方法都要求 conv_id
  const std::string conv = "mem-conv-1";

  fish::memory::Fact f;
  f.id = "f1";
  f.subject = "偏好";
  f.content = "喜欢短回复";
  f.confidence = 0.9;
  f.created = "2026-11-13";
  auto e = notes.append_fact(conv, f);
  CHECK(e.ok(), "append_fact succeeds");

  auto loaded = notes.load_facts(conv);
  CHECK(loaded.ok() && loaded.value->size() == 1, "fact roundtrip count");
  CHECK(loaded.ok() && (*loaded.value)[0].content == "喜欢短回复", "fact content roundtrip");
  CHECK(loaded.ok() && (*loaded.value)[0].subject == "偏好", "fact subject roundtrip");

  fish::memory::Episode ep;
  ep.id = "e1";
  ep.summary = "聊了加班的事";
  ep.importance = 0.7;
  ep.created = "2026-11-13";
  CHECK(notes.append_episode(conv, ep).ok(), "append_episode succeeds");

  auto eps = notes.load_episodes(conv);
  CHECK(eps.ok() && eps.value->size() == 1, "episode roundtrip");

  std::vector<fish::memory::Trait> traits{{"不喜欢被追问细节", 4}};
  CHECK(notes.save_traits(conv, traits).ok(), "save_traits succeeds");
  auto loaded_traits = notes.load_traits(conv);
  CHECK(loaded_traits.ok() && loaded_traits.value->size() == 1, "traits roundtrip");
  CHECK(loaded_traits.ok() && (*loaded_traits.value)[0].evidence == 4, "trait evidence");

  // 会话隔离：另一个会话读不到这个会话的记忆
  auto other = notes.load_facts("别的会话");
  CHECK(other.ok() && other.value->empty(), "other session sees nothing");

  fs::remove_all(dir);
}

void test_index_search() {
  const std::string db = "/tmp/fish_test_" + std::to_string(std::rand()) + ".db";
  fish::memory::IndexStore idx(db);
  CHECK(idx.open().ok(), "index opens");

  idx.upsert_item("c1", "fact", "f1", "她喜欢猫", {1.0F, 0.0F, 0.0F});
  idx.upsert_item("c1", "fact", "f2", "她讨厌加班", {0.0F, 1.0F, 0.0F});

  auto r = idx.search({0.95F, 0.05F, 0.0F}, 1, "c1");
  CHECK(r.ok(), "search succeeds");
  CHECK(r.ok() && r.value->size() == 1, "search returns top_k");
  CHECK(r.ok() && (*r.value)[0].id == "f1", "search finds semantically closest");

  auto kw = idx.search_text("c1", "加班", 5);
  CHECK(kw.ok() && kw.value->size() == 1, "keyword search finds exact match");

  idx.close();
  fs::remove(db);
  fs::remove(db + "-wal");
  fs::remove(db + "-shm");
}

void test_index_rebuild_from_notes() {
  const std::string base = "/tmp/fish_rebuild_" + std::to_string(std::rand());
  const std::string notes_dir = base + "/notes";
  const std::string db = base + "/index.db";

  StubEmbedding emb;

  // 第一轮：写入并建立索引
  {
    fish::memory::MemorySystem::Config c;
    c.notes_dir = notes_dir;
    c.db_path = db;
    fish::memory::MemorySystem ms(c, &emb);
    CHECK(ms.init().ok(), "memory init");

    // 直接写旁路文件，模拟已有记忆
    fish::memory::Fact f;
    f.id = "f1";
    f.content = "她养了一只猫";
    f.confidence = 0.9;
    f.created = "2026-11-13";
    ms.notes().ensure_dirs();
    ms.notes().append_fact("rebuild-conv", f);
  }

  // 第二轮：删除索引，验证从文件重建
  fs::remove(db);
  fs::remove(db + "-wal");
  fs::remove(db + "-shm");

  {
    fish::memory::MemorySystem::Config c;
    c.notes_dir = notes_dir;
    c.db_path = db;
    fish::memory::MemorySystem ms(c, &emb);
    CHECK(ms.init().ok(), "memory re-init after index loss");
    CHECK(ms.index().item_count() > 0, "index rebuilt from notes files");
  }

  fs::remove_all(base);
}

void test_consolidate_json_apply() {
  const std::string dir = "/tmp/fish_cons_" + std::to_string(std::rand());
  const std::string db = dir + "/i.db";
  fish::memory::NotesStore notes(dir);
  notes.ensure_dirs();
  fish::memory::IndexStore idx(db);
  idx.open();
  StubEmbedding emb;

  const std::string model_out = R"({
    "episodes": [{"summary": "聊了加班", "importance": 0.8}],
    "facts": [{"subject": "用户", "content": "在一家芯片公司上班", "confidence": 0.9}],
    "traits": [{"pattern": "不喜欢被追问", "evidence": 2}]
  })";

  auto r = fish::memory::Consolidator::apply_json(model_out, "2026-11-13", &notes, &idx, &emb, CONV);
  CHECK(r.ok(), "apply_json succeeds");
  CHECK(r.ok() && r.value->episodes_written == 1, "one episode written");
  CHECK(r.ok() && r.value->facts_written == 1, "one fact written");

  auto facts = notes.load_facts(CONV);
  CHECK(facts.ok() && facts.value->size() == 1, "fact persisted to sidecar file");
  CHECK(idx.item_count() == 2, "index has episode + fact");

  // 证据累加：再来一次同样 trait
  const std::string model_out2 = R"({"episodes":[],"facts":[],"traits":[{"pattern":"不喜欢被追问","evidence":2}]})";
  fish::memory::Consolidator::apply_json(model_out2, "2026-11-14", &notes, &idx, &emb, CONV);
  auto traits = notes.load_traits(CONV);
  CHECK(traits.ok() && (*traits.value)[0].evidence == 4, "trait evidence accumulates");

  idx.close();
  fs::remove_all(dir);
}

void test_gather_includes_working_and_longterm() {
  const std::string base = "/tmp/fish_gather_" + std::to_string(std::rand());
  StubEmbedding emb;

  // MemorySystem 在 remove_all 之前销毁（关掉 SQLite 句柄）。
  {
    fish::memory::MemorySystem::Config c;
    c.notes_dir = base + "/notes";
    c.db_path = base + "/i.db";
    fish::memory::MemorySystem ms(c, &emb);
    ms.init();

    // 写一条长期事实
    auto j =
        R"({"episodes":[],"facts":[{"subject":"用户","content":"养了一只叫豆豆的猫","confidence":0.9}],"traits":[]})";
    fish::memory::Consolidator::apply_json(j, "2026-11-13", &ms.notes(), &ms.index(), &emb,
                                            CONV);

    // 记录短时
    fish::memory::Message u, a;
    u.role = "user";
    u.text = "我家猫今天又拆家了";
    u.at = fish::core::Clock::now();
    a.role = "assistant";
    a.text = "豆豆又淘气了";
    a.at = fish::core::Clock::now();
    ms.record("conv1", u, a);

    auto ctx = ms.gather("conv1");
    CHECK(ctx.ok(), "gather succeeds");
    CHECK(ctx.ok() && !ctx.value->working.empty(), "gather includes working memory");

    // 长期记忆不进 gather 的 working —— 走检索，由模型自己取。
    //
    // 注意用 CONV 检索：事实是写进 CONV 会话的。检索按会话隔离，
    // 用别的会话 id 搜是搜不到的（这正是缺陷 2 修好的行为）。
    auto hits = ms.search(CONV, "豆豆", 5);
    CHECK(hits.ok() && !hits.value->empty(), "long-term fact is retrievable via search");
  }
  fs::remove_all(base);
}

// ── 缺陷 1：多行 episode 往返 ────────────────────────────
//
// compact() 落的是整段多行 transcript，旧实现直接写进文件，
// load_episodes() 按行解析只能读回第一行（实测 367 字节 → 24 字节）。
// 修复后：正文转义成单行（换行 → 字面量 "\n"），读回来 100% 还原。
void test_multiline_episode_roundtrip() {
  const std::string dir = "/tmp/fish_test_ml_" + std::to_string(std::rand());
  fish::memory::NotesStore notes(dir);
  notes.ensure_dirs();

  const std::string conv = "ml-conv";
  const std::string multiline =
      "对方: 第 0 条消息，我在一家做芯片的公司上班\n"
      "我: 知道了，第 0 条\n"
      "对方: 第 1 条消息\n"
      "我: 知道了，第 1 条\n"
      "对方: 反斜杠 \\ 和百分号 % 也要能还原";

  fish::memory::Episode ep;
  ep.id = "e-ml";
  ep.summary = multiline;
  ep.importance = 0.5;
  ep.created = "2026-11-15";
  CHECK(notes.append_episode(conv, ep).ok(), "append multiline episode");

  auto eps = notes.load_episodes(conv);
  CHECK(eps.ok() && eps.value->size() == 1, "multiline episode parsed as one entry");
  CHECK(eps.ok() && (*eps.value)[0].summary == multiline,
        "multiline episode content identical after roundtrip");
  CHECK(eps.ok() && (*eps.value)[0].importance == 0.5, "multiline episode importance kept");
  CHECK(eps.ok() && (*eps.value)[0].created == "2026-11-15", "multiline episode date kept");

  // 旧格式兼容：单行 episode 仍然能读（转义后没有特殊字符，读回来一样）
  fish::memory::Episode single;
  single.id = "e-single";
  single.summary = "聊了加班的事";
  single.importance = 0.7;
  single.created = "2026-11-13";
  CHECK(notes.append_episode(conv, single).ok(), "append single-line episode");

  auto eps2 = notes.load_episodes(conv);
  CHECK(eps2.ok() && eps2.value->size() == 2, "both episodes parsed");
  bool found_single = false;
  if (eps2.ok()) {
    for (const auto& e : *eps2.value) {
      if (e.id == "e-single" && e.summary == "聊了加班的事") found_single = true;
    }
  }
  CHECK(found_single, "single-line episode still readable (backward compat)");

  // 旧格式（缺陷 1 修复前写出的多行条目）也要能读：
  // 元数据单独占一行，正文是多行 transcript。
  const std::string legacy_dir = "/tmp/fish_test_legacy_" + std::to_string(std::rand());
  fish::memory::NotesStore legacy(legacy_dir);
  legacy.ensure_dirs();
  {
    const std::string sdir = legacy.session_dir(conv);
    fs::create_directories(sdir + "/episodes");
    std::FILE* f = std::fopen((sdir + "/episodes/2026-11.md").c_str(), "w");
    CHECK(f != nullptr, "legacy episode file created");
    if (f != nullptr) {
      std::fputs("- [id:e-legacy] 对方: 第一行\n我: 第二行\n  (重要度 0.5, 2026-11-15)\n", f);
      std::fclose(f);
    }
  }
  auto legacy_eps = legacy.load_episodes(conv);
  CHECK(legacy_eps.ok() && legacy_eps.value->size() == 1, "legacy multiline entry parsed");
  CHECK(legacy_eps.ok() && (*legacy_eps.value)[0].summary == "对方: 第一行\n我: 第二行",
        "legacy multiline content recovered");
  CHECK(legacy_eps.ok() && (*legacy_eps.value)[0].importance == 0.5,
        "legacy metadata line parsed");

  fs::remove_all(dir);
  fs::remove_all(legacy_dir);
}

// ── 缺陷 2：跨会话隔离 ───────────────────────────────────
void test_cross_session_isolation() {
  const std::string base = "/tmp/fish_test_iso_" + std::to_string(std::rand());
  StubEmbedding emb;

  // 注意：MemorySystem 要在 remove_all 之前销毁（关掉 SQLite 句柄），
  // 否则 Windows 上删还开着的 WAL 目录会崩。
  {
    fish::memory::MemorySystem::Config c;
    c.notes_dir = base + "/notes";
    c.db_path = base + "/i.db";
    fish::memory::MemorySystem ms(c, &emb);
    CHECK(ms.init().ok(), "memory init");

    // A、B 两个会话各有同样内容的事实
    const std::string j =
        R"({"episodes":[],"facts":[{"subject":"用户","content":"她养了一只叫豆豆的猫","confidence":0.9}],"traits":[]})";
    fish::memory::Consolidator::apply_json(j, "2026-11-13", &ms.notes(), &ms.index(), &emb,
                                            "conv-A");
    fish::memory::Consolidator::apply_json(j, "2026-11-13", &ms.notes(), &ms.index(), &emb,
                                            "conv-B");

    // 同内容在两会话 → 索引里两行都在（主键含 conv_id，不再互相覆盖）
    CHECK(ms.index().item_count() == 2, "same content in two sessions keeps two index rows");

    // 从 B 会话搜「豆豆」只能搜到 B 的那条
    auto hits_b = ms.search("conv-B", "豆豆", 5);
    CHECK(hits_b.ok() && hits_b.value->size() == 1, "session B finds its own fact");
    auto hits_a = ms.search("conv-A", "豆豆", 5);
    CHECK(hits_a.ok() && hits_a.value->size() == 1, "session A finds its own fact");

    // 空 conv_id → 一律不返回（宁可搜不到，也不泄漏）
    auto hits_none = ms.search("", "豆豆", 5);
    CHECK(hits_none.ok() && hits_none.value->empty(), "empty conv_id returns nothing");

    // 另一个完全没有该内容的会话搜不到
    auto hits_c = ms.search("conv-C", "豆豆", 5);
    CHECK(hits_c.ok() && hits_c.value->empty(), "unrelated session sees nothing");
  }
  fs::remove_all(base);
}

// ── 缺陷 3：索引重建后内容与真源一致 ─────────────────────
void test_rebuild_index_content_matches_source() {
  const std::string base = "/tmp/fish_test_rb_" + std::to_string(std::rand());
  StubEmbedding emb;

  // MemorySystem 在 remove_all 之前销毁（关掉 SQLite 句柄）。
  {
    fish::memory::MemorySystem::Config c;
    c.notes_dir = base + "/notes";
    c.db_path = base + "/i.db";
    fish::memory::MemorySystem ms(c, &emb);
    CHECK(ms.init().ok(), "memory init");

    const std::string j =
        R"({"episodes":[{"summary":"聊了芯片公司","importance":0.6}],"facts":[{"subject":"用户","content":"在芯片公司上班","confidence":0.9}],"traits":[]})";
    fish::memory::Consolidator::apply_json(j, "2026-11-13", &ms.notes(), &ms.index(), &emb,
                                            "rb-conv");

    // 破坏索引：删掉所有条目
    CHECK(ms.index().clear().ok(), "index cleared");
    CHECK(ms.index().item_count() == 0, "index is empty after clear");

    // 显式重建
    auto rb = ms.rebuild_index();
    CHECK(rb.ok(), "rebuild_index succeeds");
    CHECK(rb.ok() && rb.value && *rb.value == 2, "rebuild restores both items");
    CHECK(ms.index().item_count() == 2, "index non-empty after rebuild");

    // 重建后内容与真源一致
    auto hits = ms.search("rb-conv", "芯片", 5);
    CHECK(hits.ok() && !hits.value->empty(), "rebuilt index is searchable");

    auto eps = ms.notes().load_episodes("rb-conv");
    CHECK(eps.ok() && eps.value->size() == 1, "episode still in source file");
    CHECK(eps.ok() && (*eps.value)[0].summary == "聊了芯片公司",
          "episode content identical after rebuild");
  }
  fs::remove_all(base);
}

// ── 缺陷 5：search_text 的 % / _ 转义 ────────────────────
void test_search_text_like_escaping() {
  const std::string db = "/tmp/fish_test_esc_" + std::to_string(std::rand()) + ".db";
  fish::memory::IndexStore idx(db);
  CHECK(idx.open().ok(), "index opens");

  idx.upsert_item("c1", "fact", "f1", "她喜欢猫", {1.0F, 0.0F, 0.0F});
  idx.upsert_item("c1", "fact", "f2", "她讨厌加班", {0.0F, 1.0F, 0.0F});

  // '%' 不转义的话会匹配全部
  auto pct = idx.search_text("c1", "%", 10);
  CHECK(pct.ok() && pct.value->empty(), "percent sign does not match everything");

  // '_' 不转义的话是单字符通配
  auto us = idx.search_text("c1", "_", 10);
  CHECK(us.ok() && us.value->empty(), "underscore is escaped (not a wildcard)");

  // 空关键词 → 空结果（不退化成 LIKE '%%'）
  auto empty_kw = idx.search_text("c1", "", 10);
  CHECK(empty_kw.ok() && empty_kw.value->empty(), "empty keyword returns nothing");

  // 正常关键词仍然能命中
  auto ok = idx.search_text("c1", "加班", 10);
  CHECK(ok.ok() && ok.value->size() == 1, "normal keyword still matches");

  idx.close();
  fs::remove(db);
  fs::remove(db + "-wal");
  fs::remove(db + "-shm");
}

// ── 缺陷 6：search_text 返回最相关的 N 条，而不是最先插入的 N 条 ──
void test_search_text_relevance_order() {
  const std::string db = "/tmp/fish_test_rel_" + std::to_string(std::rand()) + ".db";
  fish::memory::IndexStore idx(db);
  CHECK(idx.open().ok(), "index opens");

  // 先插入的一条「最不相关」：命中位置靠后
  idx.upsert_item("c1", "fact", "f1", "无关的开头内容，后面才提到豆豆", {1.0F, 0.0F, 0.0F});
  // 后插入的「最相关」：命中位置靠前
  idx.upsert_item("c1", "fact", "f2", "豆豆就是那只猫", {0.0F, 1.0F, 0.0F});

  auto r = idx.search_text("c1", "豆豆", 1);
  CHECK(r.ok() && r.value->size() == 1, "top_k respected");
  CHECK(r.ok() && (*r.value)[0].id == "f2",
        "keyword search returns most relevant, not first inserted");

  idx.close();
  fs::remove(db);
  fs::remove(db + "-wal");
  fs::remove(db + "-shm");
}

// ── 缺陷 4：raw_ratio 低于下限时被抬到下限 ────────────────
void test_raw_ratio_lower_bound() {
  // 配 0.01（过于激进）→ 抬到下限 0.05
  const auto low = fish::memory::TokenBudget::make(100000, 0.01, 0.25);
  CHECK(low.raw_budget ==
            static_cast<std::size_t>(100000.0 * fish::memory::TokenBudget::kRawBufferMinRatio),
        "raw ratio below minimum is clamped up to the minimum");

  // 正常值不受下限影响
  const auto normal = fish::memory::TokenBudget::make(100000, 0.40, 0.25);
  CHECK(normal.raw_budget == static_cast<std::size_t>(100000.0 * 0.40),
        "normal raw ratio unaffected by lower bound");

  // 超上限仍然被压回上限
  const auto high = fish::memory::TokenBudget::make(100000, 0.99, 0.25);
  CHECK(high.raw_budget ==
            static_cast<std::size_t>(100000.0 * fish::memory::TokenBudget::kRawBufferMaxRatio),
        "raw ratio above maximum is clamped to the maximum");
}

}  // namespace

int main() {
  std::printf("=== memory tests ===\n");
  test_working_memory_capacity();
  test_no_eviction();
  test_notes_roundtrip();
  test_multiline_episode_roundtrip();
  test_index_search();
  test_search_text_like_escaping();
  test_search_text_relevance_order();
  test_cross_session_isolation();
  test_rebuild_index_content_matches_source();
  test_index_rebuild_from_notes();
  test_consolidate_json_apply();
  test_gather_includes_working_and_longterm();
  test_raw_ratio_lower_bound();

  if (failures != 0) {
    std::printf("\n%d FAILURES\n", failures);
    return 1;
  }
  std::printf("\nAll memory tests passed.\n");
  return 0;
}
