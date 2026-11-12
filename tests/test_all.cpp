// tests/test_all.cpp
// 统一测试入口：模块化后按 import 组织。
#include <cassert>
#include <chrono>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

import fish.core;
import fish.log;
import fish.memory;
import fish.onebot_ws;
import fish.channels;
import fish.orchestrator;

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      std::printf("[FAIL] %s\n", msg);                 \
      ++g_failures;                                    \
    } else {                                           \
      std::printf("[PASS] %s\n", msg);                 \
    }                                                  \
  } while (0)

// ── 被动对话红线 ────────────────────────────────────────
fish::core::InboundMessage mk_msg(fish::core::ChatKind kind, bool mentioned) {
  fish::core::InboundMessage m;
  m.kind = kind;
  m.mentioned_me = mentioned;
  m.conversation_id = "100";
  m.message_id = "m1";
  m.received_at = fish::core::Clock::now();
  return m;
}

void test_policy() {
  std::printf("\n=== policy ===\n");
  fish::orchestrator::ReplyPolicy p;
  fish::core::ChannelCapability cap;

  auto d1 = p.evaluate(mk_msg(fish::core::ChatKind::kGroup, false), cap);
  CHECK(!d1.should_reply, "group without @ is silent");
  CHECK(d1.reason == "group_not_mentioned", "reason recorded");

  auto d2 = p.evaluate(mk_msg(fish::core::ChatKind::kGroup, true), cap);
  CHECK(d2.should_reply, "group with @ replies");

  auto d3 = p.evaluate(mk_msg(fish::core::ChatKind::kPrivate, false), cap);
  CHECK(d3.should_reply, "private always replies");

  fish::core::ChannelCapability no_priv;
  no_priv.supports_private = false;
  auto d4 = p.evaluate(mk_msg(fish::core::ChatKind::kPrivate, false), no_priv);
  CHECK(!d4.should_reply, "channel without private support rejects");
}

void test_reply_budget() {
  std::printf("\n=== reply budget ===\n");
  fish::orchestrator::ReplyBudget b(2);
  CHECK(b.remaining("m1") == 2, "initial budget is limit");
  CHECK(b.consume("m1"), "first consume ok");
  CHECK(b.consume("m1"), "second consume ok");
  CHECK(!b.consume("m1"), "third consume rejected");
  CHECK(b.remaining("m1") == 0, "budget exhausted");
  CHECK(b.remaining("m2") == 2, "other message keeps full budget");
}

void test_dedup() {
  std::printf("\n=== dedup ===\n");
  fish::orchestrator::DedupCache c;
  CHECK(!c.seen("k1"), "first sight is new");
  CHECK(c.seen("k1"), "second sight is duplicate");
  CHECK(!c.seen("k2"), "different key is new");
}

// ── 记忆三层 ────────────────────────────────────────────
class StubEmbedding final : public fish::core::IEmbeddingModel {
 public:
  int dimension() const noexcept override { return 8; }
  fish::core::Result<std::vector<float>> embed(const std::string& text) override {
    std::vector<float> v(8, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      v[i % 8] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    for (auto& x : v) x += 0.01F;
    return fish::core::Result<std::vector<float>>::ok(std::move(v));
  }
};

void test_working_memory() {
  std::printf("\n=== working memory ===\n");
  fish::memory::WorkingMemory wm(2);
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
  CHECK(wm.round_count() == 2, "keeps only max_rounds");
  auto snap = wm.snapshot();
  CHECK(snap.size() == 4, "2 rounds = 4 messages");
  CHECK(snap.front().text == "u3", "oldest evicted");
}

void test_decay_and_eviction() {
  std::printf("\n=== forgetting ===\n");
  const double w = fish::memory::Forgetting::decay(1.0, 30.0, 30.0);
  CHECK(w > 0.49 && w < 0.51, "one half-life halves weight");
  CHECK(fish::memory::Forgetting::decay(1.0, 0.0, 30.0) == 1.0, "no decay at zero");

  fish::memory::Forgetting f(fish::memory::Forgetting::Params{30.0, 0.05});
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
  facts.push_back(mk("done", 0.9, "other"));
  auto evict = f.select_evictions(facts, 400.0);
  CHECK(!evict.empty(), "very old facts evicted");
  bool done_evicted = false;
  for (const auto& id : evict) {
    if (id == "done") done_evicted = true;
  }
  CHECK(!done_evicted, "superseded facts skip eviction");
}

void test_notes_roundtrip() {
  std::printf("\n=== notes store ===\n");
  const std::string dir = "/tmp/fish_t_notes_" + std::to_string(std::rand());
  fish::memory::NotesStore notes(dir);
  notes.ensure_dirs();

  fish::memory::Fact f;
  f.id = "f1";
  f.subject = "偏好";
  f.content = "喜欢短回复";
  f.confidence = 0.9;
  f.created = "2026-11-13";
  CHECK(notes.append_fact(f).ok(), "append_fact ok");

  auto loaded = notes.load_facts();
  CHECK(loaded.ok() && loaded.value->size() == 1, "fact persisted");
  CHECK(loaded.ok() && (*loaded.value)[0].content == "喜欢短回复", "content roundtrip");
  CHECK(loaded.ok() && (*loaded.value)[0].subject == "偏好", "subject roundtrip");

  fish::memory::Episode ep;
  ep.id = "e1";
  ep.summary = "聊了加班";
  ep.importance = 0.7;
  ep.created = "2026-11-13";
  CHECK(notes.append_episode(ep).ok(), "append_episode ok");
  auto eps = notes.load_episodes();
  CHECK(eps.ok() && eps.value->size() == 1, "episode persisted");

  std::vector<fish::memory::Trait> traits{{"不喜欢被追问细节", 4}};
  CHECK(notes.save_traits(traits).ok(), "save_traits ok");
  auto lt = notes.load_traits();
  CHECK(lt.ok() && (*lt.value)[0].evidence == 4, "trait evidence roundtrip");

  fs::remove_all(dir);
}

void test_index_store() {
  std::printf("\n=== index store ===\n");
  const std::string db = "/tmp/fish_t_idx_" + std::to_string(std::rand()) + ".db";
  fish::memory::IndexStore idx(db);
  CHECK(idx.open().ok(), "index opens");

  idx.upsert_item("fact", "f1", "她喜欢猫", {1.0F, 0.0F, 0.0F});
  idx.upsert_item("fact", "f2", "她讨厌加班", {0.0F, 1.0F, 0.0F});

  auto r = idx.search({0.95F, 0.05F, 0.0F}, 1);
  CHECK(r.ok() && r.value->size() == 1, "search returns top_k");
  CHECK(r.ok() && (*r.value)[0].id == "f1", "finds closest by cosine");

  auto kw = idx.search_text("加班", 5);
  CHECK(kw.ok() && kw.value->size() == 1, "keyword search works");

  idx.close();
  fs::remove(db);
  fs::remove(db + "-wal");
  fs::remove(db + "-shm");
}

void test_consolidation() {
  std::printf("\n=== consolidation ===\n");
  const std::string dir = "/tmp/fish_t_cons_" + std::to_string(std::rand());
  fish::memory::NotesStore notes(dir);
  notes.ensure_dirs();
  fish::memory::IndexStore idx(dir + "/i.db");
  idx.open();
  StubEmbedding emb;

  const std::string out = R"({
    "episodes":[{"summary":"聊了加班","importance":0.8}],
    "facts":[{"subject":"用户","content":"在芯片公司上班","confidence":0.9}],
    "traits":[{"pattern":"不喜欢被追问","evidence":2}]
  })";

  auto r = fish::memory::Consolidator::apply_json(out, "2026-11-13", &notes, &idx, &emb);
  CHECK(r.ok(), "apply_json ok");
  CHECK(r.ok() && r.value->episodes_written == 1, "one episode");
  CHECK(r.ok() && r.value->facts_written == 1, "one fact");

  auto facts = notes.load_facts();
  CHECK(facts.ok() && facts.value->size() == 1, "fact in sidecar file");
  CHECK(idx.item_count() == 2, "index has episode + fact");

  // 证据累加
  const std::string out2 = R"({"episodes":[],"facts":[],"traits":[{"pattern":"不喜欢被追问","evidence":2}]})";
  fish::memory::Consolidator::apply_json(out2, "2026-11-14", &notes, &idx, &emb);
  auto traits = notes.load_traits();
  CHECK(traits.ok() && (*traits.value)[0].evidence == 4, "trait evidence accumulates");

  idx.close();
  fs::remove_all(dir);
}

void test_index_rebuild() {
  std::printf("\n=== index rebuild from notes ===\n");
  const std::string base = "/tmp/fish_t_rb_" + std::to_string(std::rand());
  StubEmbedding emb;

  {
    fish::memory::MemorySystem::Config c;
    c.notes_dir = base + "/notes";
    c.db_path = base + "/i.db";
    fish::memory::MemorySystem ms(c, &emb);
    CHECK(ms.init().ok(), "init ok");
    fish::memory::Fact f;
    f.id = "f1";
    f.content = "她养了一只猫";
    f.confidence = 0.9;
    f.created = "2026-11-13";
    ms.notes().append_fact(f);
  }

  // 删除索引，验证从旁路文件重建
  fs::remove(base + "/i.db");
  fs::remove(base + "/i.db-wal");
  fs::remove(base + "/i.db-shm");

  {
    fish::memory::MemorySystem::Config c;
    c.notes_dir = base + "/notes";
    c.db_path = base + "/i.db";
    fish::memory::MemorySystem ms(c, &emb);
    CHECK(ms.init().ok(), "re-init ok");
    CHECK(ms.index().item_count() > 0, "index rebuilt from files");
  }

  fs::remove_all(base);
}

void test_gather() {
  std::printf("\n=== gather (L2 + L3) ===\n");
  const std::string base = "/tmp/fish_t_g_" + std::to_string(std::rand());
  StubEmbedding emb;

  fish::memory::MemorySystem::Config c;
  c.notes_dir = base + "/notes";
  c.db_path = base + "/i.db";
  fish::memory::MemorySystem ms(c, &emb);
  ms.init();

  auto j = R"({"episodes":[],"facts":[{"subject":"用户","content":"养了一只叫豆豆的猫","confidence":0.9}],"traits":[]})";
  fish::memory::Consolidator::apply_json(j, "2026-11-13", &ms.notes(), &ms.index(), &emb);

  fish::memory::Message u, a;
  u.role = "user";
  u.text = "我家猫今天又拆家了";
  u.at = fish::core::Clock::now();
  a.role = "assistant";
  a.text = "豆豆又淘气了";
  a.at = fish::core::Clock::now();
  ms.record("conv1", u, a);

  auto ctx = ms.gather("conv1", "猫");
  CHECK(ctx.ok(), "gather ok");
  CHECK(ctx.ok() && !ctx.value->working.empty(), "includes working memory");
  CHECK(ctx.ok() && !ctx.value->facts.empty(), "includes long-term facts");

  fs::remove_all(base);
}

// ── 传输层 ──────────────────────────────────────────────
void test_onebot_parsing() {
  std::printf("\n=== onebot parsing ===\n");
  fish::onebot_ws::Config cfg;
  cfg.self_id = "10086";
  fish::onebot_ws::Client client(cfg);

  const std::string raw = R"({
    "post_type":"message","message_type":"group","group_id":123456,
    "user_id":999,"message_id":555,"sender":{"nickname":"张三"},
    "message":[{"type":"at","data":{"qq":"10086"}},{"type":"text","data":{"text":" 在吗"}}]
  })";
  auto r = client.parse_event(raw);
  CHECK(r.ok(), "group message parsed");
  CHECK(r.ok() && r.value->is_group, "is group");
  CHECK(r.ok() && r.value->mentioned, "@ self detected");
  CHECK(r.ok() && r.value->group_id == "123456", "group id");
  CHECK(r.ok() && r.value->nickname == "张三", "sender name");

  const std::string no_at = R"({
    "post_type":"message","message_type":"group","group_id":1,
    "user_id":2,"message_id":3,"sender":{"nickname":"A"},
    "message":[{"type":"text","data":{"text":"随便聊聊"}}]
  })";
  auto r2 = client.parse_event(no_at);
  CHECK(r2.ok() && !r2.value->mentioned, "no @ not mentioned");

  const std::string at_other = R"({
    "post_type":"message","message_type":"group","group_id":1,
    "user_id":2,"message_id":3,"sender":{"nickname":"A"},
    "message":[{"type":"at","data":{"qq":"99999"}},{"type":"text","data":{"text":" hi"}}]
  })";
  auto r3 = client.parse_event(at_other);
  CHECK(r3.ok() && !r3.value->mentioned, "@ others is not mention");

  const std::string priv = R"({
    "post_type":"message","message_type":"private","user_id":777,
    "message_id":888,"sender":{"nickname":"B"},
    "message":[{"type":"text","data":{"text":"你好"}}]
  })";
  auto r4 = client.parse_event(priv);
  CHECK(r4.ok() && !r4.value->is_group, "private parsed");

  const std::string img = R"({
    "post_type":"message","message_type":"group","group_id":1,
    "user_id":2,"message_id":3,"sender":{"nickname":"A"},
    "message":[{"type":"image","data":{"url":"https://x/y.jpg"}}]
  })";
  auto r5 = client.parse_event(img);
  bool has_image = false;
  if (r5.ok()) {
    for (const auto& s : r5.value->segments) if (s.type == "image") has_image = true;
  }
  CHECK(has_image, "image segment parsed");

  CHECK(!client.parse_event(R"({"post_type":"notice"})").ok(), "non-message rejected");
  CHECK(!client.parse_event("{bad").ok(), "bad json rejected");
}
void test_capability_decl() {
  std::printf("\n=== capability declarations ===\n");
  fish::channels::OneBotConfig oc;
  fish::channels::QQOneBotChannel onebot(oc);
  const auto cap = onebot.capability();
  CHECK(cap.supports_group, "onebot supports group");
  CHECK(cap.supports_voice_out, "onebot supports voice");
  CHECK(cap.max_replies_per_message > 5, "onebot loose budget");

  fish::channels::LocalChannel local;
  CHECK(local.capability().supports_private, "local supports private");
}

}  // namespace

int main() {
  fish::log::Logger::instance().set_level(fish::log::LogLevel::kWarn);

  test_policy();
  test_reply_budget();
  test_dedup();
  test_working_memory();
  test_decay_and_eviction();
  test_notes_roundtrip();
  test_index_store();
  test_consolidation();
  test_index_rebuild();
  test_gather();
  test_onebot_parsing();
  test_capability_decl();

  std::printf("\n");
  if (g_failures != 0) {
    std::printf("%d FAILURES\n", g_failures);
    return 1;
  }
  std::printf("All %s tests passed.\n", "suites");
  return 0;
}




