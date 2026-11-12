// tests/test_memory.cpp
// 三层记忆的核心行为测试。
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "fish/core/interfaces.hpp"
#include "fish/memory/memory_system.hpp"

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

void test_decay() {
  // 30 天不用，权重减半
  const double w = fish::memory::Forgetting::decay(1.0, 30.0, 30.0);
  CHECK(w > 0.49 && w < 0.51, "decay halves after one half-life");

  // 0 天不衰减
  CHECK(fish::memory::Forgetting::decay(1.0, 0.0, 30.0) == 1.0, "no decay at zero days");
}

void test_eviction_selection() {
  fish::memory::Forgetting f({30.0, 0.05, 100, 100});
  std::vector<fish::memory::Fact> facts;

  auto mk = [](const std::string& id, double conf, const std::string& sup) {
    fish::memory::Fact x;
    x.id = id;
    x.content = id;
    x.confidence = conf;
    x.superseded_by = sup;
    return x;
  };

  facts.push_back(mk("high", 0.95, ""));
  facts.push_back(mk("low", 0.01, ""));
  facts.push_back(mk("done", 0.9, "other"));  // 已被取代，不参与

  // 200 天后：high 0.95*0.5^(200/30) 很小，会被淘汰；superseded 不参与
  auto evict = f.select_evictions(facts, 200.0);
  CHECK(!evict.empty(), "very old facts get evicted");
  bool has_done = false;
  for (const auto& id : evict) {
    if (id == "done") has_done = true;
  }
  CHECK(!has_done, "superseded facts are not evicted (already inactive)");
}

void test_notes_roundtrip() {
  const std::string dir = "/tmp/fish_test_notes_" + std::to_string(std::rand());
  fish::memory::NotesStore notes(dir);
  notes.ensure_dirs();

  fish::memory::Fact f;
  f.id = "f1";
  f.subject = "偏好";
  f.content = "喜欢短回复";
  f.confidence = 0.9;
  f.created = "2026-11-13";
  auto e = notes.append_fact(f);
  CHECK(e.ok(), "append_fact succeeds");

  auto loaded = notes.load_facts();
  CHECK(loaded.ok() && loaded.value->size() == 1, "fact roundtrip count");
  CHECK(loaded.ok() && (*loaded.value)[0].content == "喜欢短回复", "fact content roundtrip");
  CHECK(loaded.ok() && (*loaded.value)[0].subject == "偏好", "fact subject roundtrip");

  fish::memory::Episode ep;
  ep.id = "e1";
  ep.summary = "聊了加班的事";
  ep.importance = 0.7;
  ep.created = "2026-11-13";
  CHECK(notes.append_episode(ep).ok(), "append_episode succeeds");

  auto eps = notes.load_episodes();
  CHECK(eps.ok() && eps.value->size() == 1, "episode roundtrip");

  std::vector<fish::memory::Trait> traits{{"不喜欢被追问细节", 4}};
  CHECK(notes.save_traits(traits).ok(), "save_traits succeeds");
  auto loaded_traits = notes.load_traits();
  CHECK(loaded_traits.ok() && loaded_traits.value->size() == 1, "traits roundtrip");
  CHECK(loaded_traits.ok() && (*loaded_traits.value)[0].evidence == 4, "trait evidence");

  fs::remove_all(dir);
}

void test_index_search() {
  const std::string db = "/tmp/fish_test_" + std::to_string(std::rand()) + ".db";
  fish::memory::IndexStore idx(db);
  CHECK(idx.open().ok(), "index opens");

  idx.upsert_item("fact", "f1", "她喜欢猫", {1.0F, 0.0F, 0.0F});
  idx.upsert_item("fact", "f2", "她讨厌加班", {0.0F, 1.0F, 0.0F});

  auto r = idx.search({0.95F, 0.05F, 0.0F}, 1);
  CHECK(r.ok(), "search succeeds");
  CHECK(r.ok() && r.value->size() == 1, "search returns top_k");
  CHECK(r.ok() && (*r.value)[0].id == "f1", "search finds semantically closest");

  auto kw = idx.search_text("加班", 5);
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
    ms.notes().append_fact(f);
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

  auto r = fish::memory::Consolidator::apply_json(model_out, "2026-11-13", &notes, &idx, &emb);
  CHECK(r.ok(), "apply_json succeeds");
  CHECK(r.ok() && r.value->episodes_written == 1, "one episode written");
  CHECK(r.ok() && r.value->facts_written == 1, "one fact written");

  auto facts = notes.load_facts();
  CHECK(facts.ok() && facts.value->size() == 1, "fact persisted to sidecar file");
  CHECK(idx.item_count() == 2, "index has episode + fact");

  // 证据累加：再来一次同样 trait
  const std::string model_out2 = R"({"episodes":[],"facts":[],"traits":[{"pattern":"不喜欢被追问","evidence":2}]})";
  fish::memory::Consolidator::apply_json(model_out2, "2026-11-14", &notes, &idx, &emb);
  auto traits = notes.load_traits();
  CHECK(traits.ok() && (*traits.value)[0].evidence == 4, "trait evidence accumulates");

  idx.close();
  fs::remove_all(dir);
}

void test_gather_includes_working_and_longterm() {
  const std::string base = "/tmp/fish_gather_" + std::to_string(std::rand());
  StubEmbedding emb;

  fish::memory::MemorySystem::Config c;
  c.notes_dir = base + "/notes";
  c.db_path = base + "/i.db";
  c.working_rounds = 20;
  fish::memory::MemorySystem ms(c, &emb);
  ms.init();

  // 写一条长期事实
  auto j = R"({"episodes":[],"facts":[{"subject":"用户","content":"养了一只叫豆豆的猫","confidence":0.9}],"traits":[]})";
  fish::memory::Consolidator::apply_json(j, "2026-11-13", &ms.notes(), &ms.index(), &emb);

  // 记录短时
  fish::memory::Message u, a;
  u.role = "user";
  u.text = "我家猫今天又拆家了";
  u.at = fish::core::Clock::now();
  a.role = "assistant";
  a.text = "豆豆又淘气了";
  a.at = fish::core::Clock::now();
  ms.record("conv1", u, a);

  auto ctx = ms.gather("conv1", "猫");
  CHECK(ctx.ok(), "gather succeeds");
  CHECK(ctx.ok() && !ctx.value->working.empty(), "gather includes working memory");
  CHECK(ctx.ok() && !ctx.value->facts.empty(), "gather includes long-term facts");

  fs::remove_all(base);
}

}  // namespace

int main() {
  std::printf("=== memory tests ===\n");
  test_working_memory_capacity();
  test_decay();
  test_eviction_selection();
  test_notes_roundtrip();
  test_index_search();
  test_index_rebuild_from_notes();
  test_consolidate_json_apply();
  test_gather_includes_working_and_longterm();

  if (failures != 0) {
    std::printf("\n%d FAILURES\n", failures);
    return 1;
  }
  std::printf("\nAll memory tests passed.\n");
  return 0;
}
