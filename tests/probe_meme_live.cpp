// tests/probe_meme_live.cpp
// 普通 TU：先 include 标准库，再 import 模块（模块必须在 include 之后）。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

import fish.core;
import fish.log;
import fish.meme;
import fish.tool;

namespace fs = std::filesystem;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& name, const std::string& detail = "") {
  if (ok) {
    ++g_pass;
    std::printf("[PASS] %s\n", name.c_str());
  } else {
    ++g_fail;
    std::printf("[FAIL] %s   %s\n", name.c_str(), detail.c_str());
  }
}

std::string make_dir(const std::string& tag) {
  const std::string d = "/tmp/fish_live_" + tag + "_" + std::to_string(std::rand());
  fs::create_directories(d);
  return d;
}

// 写一个**旧格式**（6 列，没有 edited）的 memes.tsv。
//
// 这条很关键：线上 data/memes/memes.tsv 的表头就是 6 列的，
// 任何读旧文件的代码路径都必须能读，否则升级后库直接空掉。
void write_legacy_tsv(const std::string& dir) {
  std::ofstream out(dir + "/memes.tsv", std::ios::trunc);
  out << "# 网络热梗库\n";
  out << "# 格式: term<TAB>meaning<TAB>usage<TAB>origin<TAB>added_at<TAB>hits"
         "<TAB>aliases\n";
  out << "term\tmeaning\tusage\torigin\tadded_at\thits\taliases\n";
  out << "yyds\t永远的神，夸人很厉害\t「这波操作yyds」\tweb\t2026-01-01\t3\tYYDS|永远滴神\n";
  out << "尊嘟假嘟\t「真的假的」的可爱说法\t「尊嘟假嘟啊」\tweb\t2026-02-02\t1\t\n";
  out << "绝绝子\t表示程度很高\t\tweb\t2026-03-03\t0\t\n";
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  fish::log::Logger::instance().set_level(fish::log::LogLevel::kError);

  const std::string dir = make_dir("live");

  // ── 1. 读旧格式文件 ───────────────────────────────────
  std::printf("\n=== 1. 旧格式（6 列）TSV 兼容 ===\n");
  write_legacy_tsv(dir);
  {
    fish::meme::MemeStore::Config cfg;
    cfg.dir = dir;
    fish::meme::MemeStore store(cfg);
    const auto e = store.init();
    check(e.ok(), "旧格式文件能加载", e.detail);
    check(store.size() == 3, "读到 3 条", std::to_string(store.size()));

    fish::meme::MemeStore::Match m;
    check(store.find("yyds", &m), "旧文件里的条目能查到");
    check(m.entry.hits == 3, "旧文件的 hits 原样保留",
          std::to_string(m.entry.hits));
    check(m.entry.aliases.size() == 2, "旧文件的别名原样保留",
          std::to_string(m.entry.aliases.size()));
    check(!m.entry.edited, "旧文件默认 edited = false");
  }

  // ── 2. 真实装配：与 main.cpp 第 620 段一模一样的接线 ──
  std::printf("\n=== 2. 按 main.cpp 真实装配 lookup_meme / fix_meme ===\n");
  {
    fish::meme::MemeStore::Config cfg;
    cfg.dir = dir;
    fish::meme::MemeStore store(cfg);
    store.init();
    auto* ms = &store;

    // 这里故意把 remote 置空：线上 search provider 配了但 key 没就绪，
    // 走的正是「没有联网后端」这条分支。验证它给模型的文案是清楚的。
    fish::tool::MemeLookupTool::RemoteSearcher remote;

    fish::tool::MemeLookupTool lookup(
        [ms](const std::string& term, fish::tool::MemeHit* out) {
          if (out == nullptr) return false;
          fish::meme::MemeStore::Match m;
          if (!ms->find(term, &m)) return false;
          out->term = m.entry.term;
          out->meaning = m.entry.meaning;
          out->usage = m.entry.usage;
          out->source = "local";
          out->percent = m.percent;
          for (std::size_t i = 0; i < m.entry.aliases.size(); ++i) {
            if (i > 0) out->aliases += "、";
            out->aliases += m.entry.aliases[i];
          }
          return true;
        },
        remote,
        [ms](const std::vector<fish::tool::MemeHit>& hits) {
          std::vector<fish::meme::Entry> entries;
          for (const auto& h : hits) {
            fish::meme::Entry e;
            e.term = h.term;
            e.meaning = h.meaning;
            e.usage = h.usage;
            e.origin = h.source.empty() ? "web" : h.source;
            entries.push_back(std::move(e));
          }
          return ms->import_many(entries);
        },
        nullptr);

    fish::tool::MemeFixTool fixer(
        [ms](const std::string& term, const std::string& meaning,
             fish::tool::MemeHit* out) {
          fish::meme::MemeStore::Match m;
          const auto e = ms->correct(term, meaning, &m);
          if (!e.ok()) return false;
          if (out != nullptr) {
            out->term = m.entry.term;
            out->meaning = m.entry.meaning;
            out->usage = m.entry.usage;
            out->source = "local";
            out->percent = m.percent;
          }
          return true;
        });

    // 2a) 本地命中，零联网
    fish::tool::Call c;
    c.name = "lookup_meme";
    c.conversation_id = "g:live_test";
    c.arguments_json = R"({"term":"yyds"})";
    auto out = lookup.run(c);
    check(out.ok, "lookup 本地命中 ok");
    check(out.content.find("永远的神") != std::string::npos, "拿到真实释义",
          out.content);
    check(out.content.find("本地热梗库") != std::string::npos, "来源：本地热梗库");
    check(out.content.find("100%") != std::string::npos, "匹配度 100%", out.content);
    check(out.content.find("也叫 YYDS") != std::string::npos,
          "展示真实别名", out.content);

    // 2b) 别名也能命中
    c.arguments_json = R"({"term":"永远滴神"})";
    out = lookup.run(c);
    check(out.ok && out.content.find("永远的神") != std::string::npos,
          "用别名「永远滴神」也能查到", out.content);
    check(out.content.find("100%") != std::string::npos,
          "别名命中也是 100%", out.content);

    // 2c) 大小写 / 全角
    c.arguments_json = R"({"term":"YYDS"})";
    out = lookup.run(c);
    check(out.ok && out.content.find("永远的神") != std::string::npos,
          "大写 YYDS 命中", out.content);

    // 2d) 库里没有 → 无联网后端，文案要说清
    c.arguments_json = R"({"term":"赛博菩萨"})";
    out = lookup.run(c);
    check(out.ok, "缺条目：ok（不抛异常）");
    check(out.content.find("没启用联网") != std::string::npos,
          "缺条目：明说没启用联网", out.content);
    check(out.content.find("赛博菩萨") != std::string::npos,
          "缺条目：说清是哪个词", out.content);
    check(!store.has("赛博菩萨"), "缺条目：没有被偷偷写进库");

    // 2e) 纠错：改掉真实文件的释义
    fish::tool::Call fc;
    fc.name = "fix_meme";
    fc.conversation_id = "g:live_test";
    fc.arguments_json =
        R"({"term":"绝绝子","meaning":"表示程度很高（纠正后的释义）"})";
    out = fixer.run(fc);
    check(out.ok, "fix_meme ok", out.content);
    check(out.content.find("已改正") != std::string::npos, "明说已改正", out.content);

    fish::meme::MemeStore::Match m;
    check(store.find("绝绝子", &m), "纠错后仍能查到");
    check(m.entry.meaning == "表示程度很高（纠正后的释义）",
          "纠错后释义已替换", m.entry.meaning);
    check(m.entry.edited, "纠错后 edited = true");
    check(m.entry.origin == "web", "纠错后来源保留", m.entry.origin);

    // 2f) 纠正后的释义落盘了，重新打开还是新的
    {
      fish::meme::MemeStore reopened(cfg);
      reopened.init();
      fish::meme::MemeStore::Match m2;
      check(reopened.find("绝绝子", &m2), "重新打开能查到");
      check(m2.entry.meaning == "表示程度很高（纠正后的释义）",
            "重新打开后释义仍是纠正过的", m2.entry.meaning);
      check(m2.entry.edited, "重新打开后 edited 仍为 true");
    }

    // 2g) 纠错一个不存在的词
    fc.arguments_json = R"({"term":"不存在的梗zzz","meaning":"x"})";
    out = fixer.run(fc);
    check(out.ok && out.error == "meme_not_found", "纠错不存在的词：meme_not_found",
          out.error);
    check(out.content.find("lookup_meme") != std::string::npos,
          "纠错不存在的词：引导先查", out.content);

    // 2h) 落盘文件是 8 列新格式，且仍是合法 TSV
    std::printf("\n=== 3. 落盘文件格式 ===\n");
    std::ifstream in(dir + "/memes.tsv");
    std::string header;
    std::getline(in, header);
    while (!header.empty() && header[0] == '#') std::getline(in, header);
    check(header.find("edited") != std::string::npos,
          "表头已升级到 8 列（含 edited）", header);
    int cols = 1;
    for (char ch : header) {
      if (ch == '\t') ++cols;
    }
    check(cols == 8, "表头列数 = 8", std::to_string(cols));

    // 数据行也都是 8 列（没有漏写导致错位）
    int bad_rows = 0;
    int rows = 0;
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      ++rows;
      int c2 = 1;
      for (char ch : line) {
        if (ch == '\t') ++c2;
      }
      if (c2 != 8) ++bad_rows;
    }
    check(rows == 3, "数据行数 = 3", std::to_string(rows));
    check(bad_rows == 0, "所有数据行都是 8 列", std::to_string(bad_rows));
  }

  fs::remove_all(dir);

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
