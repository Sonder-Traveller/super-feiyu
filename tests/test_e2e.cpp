// tests/test_e2e.cpp
// 端到端：多轮对话 → 触发巩固 → 旁路文件落盘 → 索引召回。
// 走真实 HTTP 链路（配合 mock_server.py），验证的不只是桩。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

import fish.core;
import fish.log;
import fish.config;
import fish.capability;
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

}  // namespace

int main(int argc, char** argv) {
  fish::log::Logger::instance().set_level(fish::log::LogLevel::kWarn);

  const std::string cfg_path = argc > 1 ? argv[1] : "tests/e2e.toml";
  auto loaded = fish::config::Config::load(cfg_path);
  if (!loaded.ok()) {
    std::printf("cannot load config %s: %s\n", cfg_path.c_str(), loaded.error.detail.c_str());
    return 1;
  }
  const auto cfg = *loaded.value;

  // 装配真实 provider（指向 mock server）
  auto chat_r = fish::capability::make_chat(fish::capability::load_chat_config(cfg));
  auto emb_r = fish::capability::make_embedding(fish::capability::load_embedding_config(cfg));
  if (!chat_r.ok() || !emb_r.ok()) {
    std::printf("provider init failed\n");
    return 1;
  }
  auto chat = std::move(*chat_r.value);
  auto emb = std::move(*emb_r.value);

  fish::memory::MemorySystem::Config mc;
  mc.notes_dir = cfg.get_or("memory.notes_dir", "/tmp/fish_e2e/notes");
  mc.db_path = cfg.get_or("memory.db_path", "/tmp/fish_e2e/i.db");

  fish::memory::MemorySystem mem(mc, emb.get());
  if (!mem.init().ok()) {
    std::printf("memory init failed\n");
    return 1;
  }
  fish::memory::Consolidator consolidator(chat.get(), &mem.notes(), &mem.index(), emb.get());

  const std::string conv = "e2e-conv";

  // 3 轮对话，第 2 轮后应触发巩固
  const std::vector<std::pair<std::string, std::string>> rounds = {
      {"我在一家做芯片的公司上班", "哦，芯片啊，挺硬核的"},
      {"最近老是加班到很晚，好累", "加班真的折磨人"},
      {"今天总算准点下班了", "那今晚可以歇会儿了"},
  };

  for (const auto& [user, bot] : rounds) {
    fish::memory::Message u, a;
    u.role = "user";
    u.text = user;
    u.at = fish::core::Clock::now();
    a.role = "assistant";
    a.text = bot;
    a.at = fish::core::Clock::now();
    mem.record(conv, u, a);

    // 超预算时压缩（按 token，不按轮数）
    if (mem.needs_compaction(conv)) {
      auto c = mem.compact(conv, consolidator);
      if (!c.ok()) {
        std::printf("[warn] compact failed: %s\n", c.error.detail.c_str());
      } else if (!c.value->skipped) {
        std::printf("  压缩触发: episodes=%d\n", c.value->episodes_written);
      }
    }
  }

  // 强制再巩固一次，确保剩余对话也落盘
  auto fin = mem.force_consolidate(conv, consolidator);
  CHECK(fin.ok(), "force_consolidate 成功");

  // 旁路文件应存在
  // 会话隔离后文件在 notes/<group|user>/<safe_id>/ 下，不再是 notes_dir 根目录。
  const std::string sdir = mem.notes().session_dir(conv);
  CHECK(fs::exists(sdir + "/facts.md"), "facts.md 已生成（旁路文件是权威源）");
  CHECK(fs::exists(sdir + "/episodes"), "episodes 目录已生成");

  // 索引应有条目
  const auto count = mem.index().item_count();
  std::printf("  索引条目数: %zu\n", count);

  // 召回：查询应与记忆内容相关
  auto ctx = mem.gather(conv, "我在哪上班");
  CHECK(ctx.ok(), "gather 成功");

  // 删除索引，验证能从旁路文件重建
  mem.index().close();
  fs::remove(mc.db_path);
  fs::remove(mc.db_path + "-wal");
  fs::remove(mc.db_path + "-shm");

  fish::memory::MemorySystem mem2(mc, emb.get());
  CHECK(mem2.init().ok(), "索引删除后重建成功");
  std::printf("  重建后条目数: %zu\n", mem2.index().item_count());
  CHECK(mem2.index().item_count() > 0, "重建后索引非空（旁路文件是真正权威）");

  std::printf("\n");
  if (g_fail != 0) {
    std::printf("%d FAILURES\n", g_fail);
    return 1;
  }
  std::printf("E2E PASSED\n");
  return 0;
}

