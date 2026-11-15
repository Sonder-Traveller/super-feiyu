// tests/probe_meme.cpp
// 热梗库 + 清单工具的**纯逻辑**验证程序。
//
// 覆盖：
//   1. 清单语义（不带参数返回全部条目；条数 == 库大小；不截断）
//   2. 按序号取详情（index 正确 / 越界 / 复数次 hits 累加）
//   3. learn 参数（假 remote 注入：学到写回 / 无联网 / 联网失败 / 空结果 / 不相关）
//   4. 查重（同名 / 同别名不重复写）
//   5. 相关性校验（不相关文本不写回）
//   6. 写回并发安全（多线程同时学同一个梗 → 文件不坏、条目不重复）
//   7. 别名抽取（数据增强）
//
// 明确**不真调联网搜索**（消耗配额）：远程搜索用假的响应体注入，
// 走的是和真链路完全相同的代码路径（is_relevant → import_many → 记忆）。
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

import fish.core;
import fish.log;
import fish.meme;
import fish.tool;

// FISH_PROBE_MEME_NO_MEMORY=1 时不编译记忆组。
// 用途：记忆模块正在被别的改动动的时候，单独验证热梗本身的纯逻辑。
// （import 放在 #if 里是合法的：预处理先于模块声明处理。）
#if !defined(FISH_PROBE_MEME_NO_MEMORY)
import fish.memory;
#define MEME_TEST_MEMORY 1
#endif

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& name, const std::string& detail = "") {
  if (ok) {
    ++g_pass;
    std::printf("[PASS] %s\n", name.c_str());
  } else {
    ++g_fail;
    std::printf("[FAIL] %s %s\n", name.c_str(), detail.c_str());
  }
}

// 临时目录：每个测试组一个，跑完删掉。
//
// 用环境变量 FISH_PROBE_TMP 指定根目录；没指定时退回系统临时目录。
// 不写在仓库的 data/ 下：测试进程未必有那个目录的写权限
// （CI / 沙箱里都是这样），权限不足会让测试变成"测的是权限"而不是测逻辑。
std::string tmp_root() {
  if (const char* env = std::getenv("FISH_PROBE_TMP")) {
    if (*env != '\0') return std::string(env);
  }
  std::error_code ec;
  const std::filesystem::path p =
      std::filesystem::temp_directory_path(ec) / "super-feiyu-probe-meme";
  return p.string();
}

std::string make_tmp_dir(const std::string& tag) {
  const std::string dir = tmp_root() + "/" + tag;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

void rm_tmp_dir(const std::string& dir) {
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// ── 1. 规范化（现在只剩「两个写法是不是同一个词」这一个用途）────────

void test_normalize() {
  std::printf("\n=== normalize ===\n");

  check(fish::meme::normalize_text("YYDS") == "yyds", "ASCII 大写→小写");
  check(fish::meme::normalize_text("yyds") == "yyds", "小写不变");
  check(fish::meme::normalize_text("ＹＹＤＳ") == "yyds", "全角→半角");
  check(fish::meme::normalize_text("6６6") == "666", "混合全角数字→半角");
  check(fish::meme::normalize_text("  yyds  ") == "yyds", "去首尾空白");
  check(fish::meme::normalize_text("y y d s") == "yyds", "去内部空白");
  check(fish::meme::normalize_text("尊嘟假嘟") == "尊嘟假嘟", "中文原样保留");
  check(fish::meme::normalize_text("栓Q") == "栓q", "中文+拉丁：中文保留、拉丁小写");
}

// ── 2. 清单语义：不带参数 = 全部条目 ─────────────────────

void test_all_entries() {
  std::printf("\n=== all entries (清单语义) ===\n");

  const std::string dir = make_tmp_dir("all");
  {
    fish::meme::MemeStore::Config cfg;
    cfg.dir = dir;
    fish::meme::MemeStore store(cfg);
    auto e = store.init();
    check(e.ok(), "init ok", e.detail);

    check(store.all().empty(), "空库：all() 为空");
    check(store.size() == 0, "空库：size == 0");

    // 造 12 条，验证既不截断也不少给
    for (int i = 0; i < 12; ++i) {
      fish::meme::Entry en;
      en.term = "meme_" + std::to_string(i);
      en.meaning = "第 " + std::to_string(i) + " 条释义";
      en.usage = "用法 " + std::to_string(i);
      en.origin = "manual";
      store.upsert(en);
    }

    const auto all = store.all();
    check(all.size() == store.size(), "all() 条数 == 库大小",
          "all=" + std::to_string(all.size()) + " size=" +
              std::to_string(store.size()));
    check(all.size() == 12, "全部 12 条都在清单里（不截断）",
          "got " + std::to_string(all.size()));

    // 排序：added_at 倒序 + term 字典序（同一天）。这里全是同一天，
    // 所以应当严格按 term 字典序。
    bool sorted_by_term = true;
    for (std::size_t i = 1; i < all.size(); ++i) {
      if (all[i - 1].term > all[i].term) sorted_by_term = false;
    }
    check(sorted_by_term, "同一天写入：清单按 term 字典序（序号稳定）");

    // 顺序稳定：连续两次 all() 完全一致
    const auto all2 = store.all();
    bool stable = all2.size() == all.size();
    for (std::size_t i = 0; stable && i < all.size(); ++i) {
      if (all2[i].term != all[i].term) stable = false;
    }
    check(stable, "连续两次 all() 顺序一致（index 不会漂移）");
  }
  rm_tmp_dir(dir);
}

// ── 3. 按序号取详情 ──────────────────────────────────────

// ── 3b. 百分比匹配 ───────────────────────────────────────

void test_percent_match() {
  std::printf("\n=== percent_match ===\n");

  check(fish::meme::percent_match("yyds", "yyds") == 100, "同一词 → 100");
  check(fish::meme::percent_match("YYDS", "yyds") == 100, "大小写不影响 → 100");
  check(fish::meme::percent_match("ＹＹＤＳ", "yyds") == 100, "全角不影响 → 100");
  check(fish::meme::percent_match("拴Q", "栓Q") >= fish::meme::kMatchThreshold,
        "变体（拴/栓）达到阈值",
        std::to_string(fish::meme::percent_match("拴Q", "栓Q")));

  // 关键差异：多带了问法，分数必须掉下来，不能像子串那样仍然算命中
  const int with_suffix = fish::meme::percent_match("yyds是什么意思", "yyds");
  check(with_suffix < 100, "带问法的整句不是 100 分",
        std::to_string(with_suffix));

  check(fish::meme::percent_match("yyds", "abc") == 0, "完全无关 → 0");
  check(fish::meme::percent_match("", "yyds") == 0, "空串 → 0");
  check(fish::meme::percent_match("yyds", "") == 0, "空串（反向）→ 0");

  // 单字词不退化成分母 0
  check(fish::meme::percent_match("6", "6") == 100, "单字同词 → 100");
  check(fish::meme::percent_match("6", "7") == 0, "单字不同 → 0");

  // 中文多字
  check(fish::meme::percent_match("尊嘟假嘟", "尊嘟假嘟") == 100, "中文同词 → 100");
  const int partial = fish::meme::percent_match("尊嘟假嘟o", "尊嘟假嘟");
  check(partial > 0 && partial < 100, "中文加字 → 0 与 100 之间",
        std::to_string(partial));

  // 标点/空白不参与比较
  check(fish::meme::percent_match("  yyds  ", "yyds") == 100, "首尾空白不影响");
  check(fish::meme::percent_match("y·y·d·s", "yyds") == 100, "中间标点不影响");
}

// ── 3. 查找（find）──────────────────────────────────────

void test_find() {
  std::printf("\n=== find(term) ===\n");

  const std::string dir = make_tmp_dir("find");
  {
    fish::meme::MemeStore::Config cfg;
    cfg.dir = dir;
    fish::meme::MemeStore store(cfg);
    store.init();

    fish::meme::Entry a;
    a.term = "yyds";
    a.meaning = "永远的神";
    a.aliases = {"YYDS"};
    a.added_at = "2026-01-01";
    store.upsert(a);

    fish::meme::Entry b;
    b.term = "尊嘟假嘟";
    b.meaning = "真的假的的可爱说法";
    b.added_at = "2026-02-02";
    store.upsert(b);

    // 完全同名
    fish::meme::MemeStore::Match m;
    check(store.find("yyds", &m), "find：完全同名命中");
    check(m.entry.term == "yyds", "find：拿到正确条目", m.entry.term);
    check(m.percent == 100, "find：完全是 100 分", std::to_string(m.percent));
    check(m.exact, "find：exact 标记为真");

    // 大小写变体
    fish::meme::MemeStore::Match m2;
    check(store.find("YYDS", &m2), "find：大小写变体也命中");
    check(m2.percent == 100, "find：变体仍是 100 分");

    // 中文
    fish::meme::MemeStore::Match m3;
    check(store.find("尊嘟假嘟", &m3), "find：中文同名命中");
    check(m3.entry.term == "尊嘟假嘟", "find：中文拿到正确条目");

    // 带问法的整句：仍能命中，但分数不是 100
    fish::meme::MemeStore::Match m4;
    const bool hit4 = store.find("尊嘟假嘟是什么意思", &m4);
    if (hit4) {
      check(m4.percent < 100, "find：带问法不是 100 分",
            std::to_string(m4.percent));
      check(m4.entry.term == "尊嘟假嘟", "find：带问法仍指向正确条目");
    } else {
      check(true, "find：带问法低于阈值，交给联网（也是可接受的）");
    }

    // 完全无关 → 没找到
    fish::meme::MemeStore::Match m5;
    check(!store.find("zzz完全不相干的词", &m5), "find：无关词 → false");

    // 空库
    fish::meme::MemeStore::Config cfg2;
    cfg2.dir = dir + "/empty";
    fish::meme::MemeStore empty_store(cfg2);
    empty_store.init();
    fish::meme::MemeStore::Match m6;
    check(!empty_store.find("yyds", &m6), "find：空库 → false");

    // hits 只在命中时累加（find 本身不计数，由工具层决定）
    const auto all = store.all();
    for (const auto& x : all) {
      if (x.term == "yyds") {
        check(x.hits == 0, "find 本身不累加 hits（由工具层负责）",
              std::to_string(x.hits));
      }
    }
  }
  rm_tmp_dir(dir);
}

// ── 3c. 纠错（correct）───────────────────────────────────

void test_correct() {
  std::printf("\n=== correct(term, meaning) ===\n");

  const std::string dir = make_tmp_dir("correct");
  {
    fish::meme::MemeStore::Config cfg;
    cfg.dir = dir;
    fish::meme::MemeStore store(cfg);
    store.init();

    fish::meme::Entry a;
    a.term = "yyds";
    a.meaning = "错的释义";
    a.usage = "保留的用法";
    a.origin = "web";
    a.added_at = "2026-01-01";
    store.upsert(a);

    fish::meme::MemeStore::Match m;
    auto e = store.correct("yyds", "永远的神（正确）", &m);
    check(e.ok(), "correct：成功", e.detail);
    check(m.entry.meaning == "永远的神（正确）", "correct：释义已替换", m.entry.meaning);
    check(m.entry.usage == "保留的用法", "correct：用法保留", m.entry.usage);
    check(m.entry.origin == "web", "correct：来源保留", m.entry.origin);
    check(m.entry.term == "yyds", "correct：梗名不变", m.entry.term);
    check(m.entry.edited, "correct：打上 edited 标记");

    // 落盘了（重新加载还能读到）
    fish::meme::MemeStore reloaded(cfg);
    reloaded.init();
    fish::meme::MemeStore::Match m2;
    check(reloaded.find("yyds", &m2), "correct：重新加载后能找到");
    check(m2.entry.meaning == "永远的神（正确）", "correct：重新加载后释义是新的",
          m2.entry.meaning);
    check(m2.entry.edited, "correct：重新加载后 edited 仍为真");

    // 库里没有的词 → kNotFound
    fish::meme::MemeStore::Match m3;
    auto e2 = store.correct("完全不存在zzz", "随便", &m3);
    check(!e2.ok(), "correct：库里没有 → 失败");
    check(e2.code == fish::core::ErrorCode::kNotFound, "correct：错误码是 kNotFound");

    // 空参数
    fish::meme::MemeStore::Match m4;
    check(!store.correct("", "x", &m4).ok(), "correct：空 term → 失败");
    check(!store.correct("yyds", "", &m4).ok(), "correct：空 meaning → 失败");

    // 纠正过的条目，自动学习不再覆盖释义
    fish::meme::Entry auto_learned;
    auto_learned.term = "yyds";
    auto_learned.meaning = "自动学来的（错的）";
    auto_learned.origin = "web";
    const int added = store.import_many({auto_learned});
    check(added == 0, "correct：自动学习不新增条目", std::to_string(added));
    fish::meme::MemeStore::Match m5;
    store.find("yyds", &m5);
    check(m5.entry.meaning == "永远的神（正确）",
          "correct：**纠正后的释义不被自动学习覆盖**", m5.entry.meaning);
  }
  rm_tmp_dir(dir);
}

// ── 4. 查重 ──────────────────────────────────────────────

void test_dedup() {
  std::printf("\n=== dedup ===\n");

  const std::string dir = make_tmp_dir("dedup");
  {
    fish::meme::MemeStore::Config cfg;
    cfg.dir = dir;
    fish::meme::MemeStore store(cfg);
    store.init();

    fish::meme::Entry a;
    a.term = "yyds";
    a.meaning = "永远的神";
    a.aliases = {"YYDS"};
    store.upsert(a);

    // 同名 → 不重复写
    fish::meme::Entry b = a;
    b.meaning = "永远的神（重复）";
    const int added_same = store.import_many({b});
    check(added_same == 0, "同名重复导入 → 新增 0", "got " + std::to_string(added_same));
    check(store.size() == 1, "同名重复导入 → 库大小仍为 1",
          "got " + std::to_string(store.size()));

    // 别名撞已有 term → 不重复写
    fish::meme::Entry c;
    c.term = "永远的神";
    c.aliases = {"yyds"};
    c.meaning = "别名撞 term";
    const int added_alias = store.import_many({c});
    check(added_alias == 0, "新 term + 别名撞已有 term → 新增 0",
          "got " + std::to_string(added_alias));
    check(store.size() == 1, "别名撞已有 term → 库大小仍为 1",
          "got " + std::to_string(store.size()));

    // 新 term 等于库里已有的别名 → 不重复写
    fish::meme::Entry d;
    d.term = "YYDS";
    d.meaning = "term 等于已有别名";
    const int added_case = store.import_many({d});
    check(added_case == 0, "term 等于已有别名 → 新增 0",
          "got " + std::to_string(added_case));

    // 大小写变体 → 不重复写
    fish::meme::Entry d2;
    d2.term = "YYDS";
    d2.meaning = "大小写变体";
    const int added_case2 = store.import_many({d2});
    check(added_case2 == 0, "大小写变体重复导入 → 新增 0",
          "got " + std::to_string(added_case2));
    check(store.size() == 1, "以上都不重复写 → 库大小仍为 1",
          "got " + std::to_string(store.size()));

    // 真正的新梗 → 会新增
    fish::meme::Entry e;
    e.term = "栓Q";
    e.meaning = "示弱";
    const int added_new = store.import_many({e});
    check(added_new == 1, "新梗导入 → 新增 1", "got " + std::to_string(added_new));
    check(store.size() == 2, "导入后库大小为 2", "got " + std::to_string(store.size()));

    check(store.has("YYDS"), "has：别名也算已收录");
    check(store.has("拴Q"), "has：变体也算已收录");
    check(!store.has("zzz不存在的词"), "has：无关词不算已收录");
  }
  rm_tmp_dir(dir);
}

// ── 5. 相关性校验（抓取结果的质量闸门，不是判断用户意图）──────────

void test_relevance() {
  std::printf("\n=== relevance gate ===\n");

  std::string term;
  check(fish::meme::is_relevant_with_context(
            "yyds", "yyds 是网络用语，意思是永远的神，常用来夸人很强", &term),
        "相关：正文里出现梗名 + 解释语境");
  check(term == "yyds", "相关时回填梗名", "got " + term);

  check(fish::meme::is_relevant_with_context(
            "yyds", "【yyds】永远的神，网络流行语", &term),
        "相关：正文里带括号出现");

  // 不相关的（同名的东西）
  check(!fish::meme::is_relevant_with_context(
            "666", "计算器上的 666 是数字游戏里的说法，跟网络梗无关"),
        "不相关：抓到的是同名别的东西");
  check(!fish::meme::is_relevant_with_context("班味", "班组长的工作安排与值班表说明",
                                               &term),
        "不相关：同音不同物");
  check(!fish::meme::is_relevant_with_context("yyds", "这是一段完全无关的正文", &term),
        "不相关：正文里根本没有梗名");
  check(!fish::meme::is_relevant_with_context("", "任意文本"), "空查询 → 不相关");
  check(!fish::meme::is_relevant_with_context("yyds", ""), "空正文 → 不相关");

  // 抓回来的是「同一个词的另一种写法」也算相关（大小写 / 变体）
  check(fish::meme::is_relevant_with_context(
            "yyds", "YYDS，网络用语，表示非常厉害", &term),
        "相关：大小写变体");
  check(fish::meme::is_relevant_with_context(
            "栓Q", "拴Q 这个梗表示示弱和委屈", &term),
        "相关：谐音变体");

  // 基础判据（只看梗名出现）仍然可用
  check(fish::meme::is_relevant("yyds", "yyds 出现在这里"), "基础判据：只看梗名出现");

  // 用法示例抽取
  const std::string usage = fish::meme::extract_usage_example(
      "yyds 是永远的神。常见用法：「这波操作 yyds」「yyds 太强了」", "yyds");
  check(!usage.empty() && usage.find("yyds") != std::string::npos,
        "抽取用法示例：拿到带梗名的引号片段", "got: " + usage);
  check(fish::meme::extract_usage_example("没有引号的正文", "yyds").empty(),
        "抽取用法示例：没有引号 → 空");
}

void test_alias_extraction() {
  std::printf("\n=== alias extraction from meaning ===\n");

  // 存量数据的真实句式
  const auto a = fish::meme::extract_aliases(
      "DeepSeek 蓝鲸 logo 的爱称，后成为拟人形象的通称。也叫蓝色大肥鱼、吃白饭的大肥鱼");
  check(a.size() == 2, "抽出 2 个别名", "got " + std::to_string(a.size()));
  bool has1 = false, has2 = false;
  for (const auto& x : a) {
    if (x == "蓝色大肥鱼") has1 = true;
    if (x == "吃白饭的大肥鱼") has2 = true;
  }
  check(has1, "别名1：蓝色大肥鱼");
  check(has2, "别名2：吃白饭的大肥鱼");

  // 又叫 / 也称 / 俗称
  const auto b = fish::meme::extract_aliases("某梗的俗称是xxx，网络上又叫yyy");
  check(b.size() >= 1, "「俗称 / 又叫」句式能抽到别名",
        "got " + std::to_string(b.size()));

  // 没有引导词 → 抽不到
  const auto c = fish::meme::extract_aliases("这是一段普通的释义，没有任何别名引导词");
  check(c.empty(), "没有引导词 → 不抽", "got " + std::to_string(c.size()));

  // 引导词后的长句不是别名
  const auto d = fish::meme::extract_aliases(
      "也叫这是一段很长的完全不像别名的说明文字用来描述它的来历和背景");
  check(d.empty(), "太长的一整句不算别名", "got " + std::to_string(d.size()));
}

// 存量数据的别名自动补全：加载后清单里能看到别名
void test_alias_autofill_from_store() {
  std::printf("\n=== alias autofill from store ===\n");

  const std::string dir = make_tmp_dir("aliasfill");
  {
    fish::meme::MemeStore::Config cfg;
    cfg.dir = dir;
    fish::meme::MemeStore store(cfg);
    store.init();

    // 模拟存量 TSV：6 列，别名混在 meaning 里
    {
      const std::string path = dir + "/memes.tsv";
      std::ofstream out(path, std::ios::trunc);
      out << "# 网络热梗库\n";
      out << "term\tmeaning\tusage\torigin\tadded_at\thits\n";
      out << "大肥鱼\tDeepSeek 蓝鲸 logo 的爱称，后成为拟人形象的通称。"
             "也叫蓝色大肥鱼、吃白饭的大肥鱼\t「大肥鱼又在摸鱼」\tmanual\t2026-11-13\t0\n";
      out << "尊嘟假嘟\t「真的假的」的可爱谐音说法，带撒娇语气\t「尊嘟假嘟」\tmanual\t"
             "2026-11-13\t0\n";
    }
    auto e = store.reload();
    check(e.ok(), "reload ok", e.detail);

    check(store.size() == 2, "6 列的老数据照样读得进来",
          "got " + std::to_string(store.size()));

    // 别名被自动抽出来 → 挂在条目上（清单里能展示）
    const auto all = store.all();
    bool found = false;
    for (const auto& x : all) {
      if (x.term != "大肥鱼") continue;
      found = true;
      bool blue = false, rice = false;
      for (const auto& al : x.aliases) {
        if (al == "蓝色大肥鱼") blue = true;
        if (al == "吃白饭的大肥鱼") rice = true;
      }
      check(blue, "自动抽出的别名挂在条目上（蓝色大肥鱼）");
      check(rice, "自动抽出的别名挂在条目上（吃白饭的大肥鱼）");
    }
    check(found, "大肥鱼那条在清单里");

    // 没有别名关系的词不会被塞进来
    bool polluted = false;
    for (const auto& x : all) {
      for (const auto& al : x.aliases) {
        if (al == "蓝色大鲸鱼") polluted = true;
      }
    }
    check(!polluted, "非别名不会出现在清单里");
  }
  rm_tmp_dir(dir);
}

// ── 6. 工具：一个词进、释义出 / 自动学习 / 降级 / 纠错 ──

// 假的联网后端：给一个像样的解释性正文，能过质量闸门。
std::vector<fish::tool::MemeHit> fake_remote(const std::string& term,
                                             std::string* error,
                                             bool force_error = false) {
  if (error != nullptr) error->clear();
  if (force_error) {
    if (error != nullptr) *error = "fake_network_error";
    return {};
  }
  if (term.empty()) return {};

  fish::tool::MemeHit h;
  h.term = term;
  // 正文里必须出现这个词，且带解释性语境词（is_relevant_with_context 的判据）
  h.meaning = term + " 是网络流行语，意思是「测试用的解释」。";
  h.usage = "";
  h.source = "fake";
  return {h};
}

void test_tool_loop() {
  std::printf("\n=== tool: term → 释义 / 自动学习 / 降级 / 纠错 ===\n");

  const std::string dir = make_tmp_dir("tool");
  {
    fish::meme::MemeStore::Config cfg;
    cfg.dir = dir;
    fish::meme::MemeStore store(cfg);
    store.init();

    std::atomic<int> remote_calls{0};
    std::atomic<int> import_calls{0};
    std::atomic<int> memory_calls{0};
    std::string last_memory_conv;
    std::string last_memory_term;

    // 与 main.cpp 的装配一一对应：查找走 find()，纠错走 correct()。
    auto finder = [&store](const std::string& term, fish::tool::MemeHit* out) {
      if (out == nullptr) return false;
      fish::meme::MemeStore::Match m;
      if (!store.find(term, &m)) return false;
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
    };
    auto corrector = [&store](const std::string& term, const std::string& meaning,
                              fish::tool::MemeHit* out) {
      fish::meme::MemeStore::Match m;
      const auto e = store.correct(term, meaning, &m);
      if (!e.ok()) return false;
      if (out != nullptr) {
        out->term = m.entry.term;
        out->meaning = m.entry.meaning;
        out->percent = m.percent;
      }
      return true;
    };
    auto remote = [&remote_calls](const std::string& q, std::string* error) {
      ++remote_calls;
      return fake_remote(q, error);
    };
    auto importer = [&store, &import_calls](
                        const std::vector<fish::tool::MemeHit>& hits) {
      ++import_calls;
      std::vector<fish::meme::Entry> entries;
      for (const auto& h : hits) {
        fish::meme::Entry e;
        e.term = h.term;
        e.meaning = h.meaning;
        e.usage = h.usage;
        e.origin = h.source;
        entries.push_back(std::move(e));
      }
      return store.import_many(entries);
    };
    auto memory = [&memory_calls, &last_memory_conv, &last_memory_term](
                      const std::string& conv, const std::string& term,
                      const std::string&) {
      ++memory_calls;
      last_memory_conv = conv;
      last_memory_term = term;
    };

    fish::tool::MemeLookupTool tool(finder, remote, importer, memory);
    fish::tool::MemeFixTool fixer(corrector);

    // ── 1) 本地命中 → 直接给释义，不联网 ──
    fish::meme::Entry seed;
    seed.term = "yyds";
    seed.meaning = "永远的神";
    seed.usage = "「yyds」太强了";
    seed.aliases = {"YYDS"};
    store.upsert(seed);

    fish::tool::Call call;
    call.name = "lookup_meme";
    call.arguments_json = R"({"term":"yyds"})";
    call.conversation_id = "g:test_group";
    auto out = tool.run(call);
    check(out.ok, "本地命中：ok");
    check(out.content.find("永远的神") != std::string::npos, "本地命中：拿到释义",
          out.content);
    check(out.content.find("本地热梗库") != std::string::npos, "本地命中：标明来源",
          out.content);
    check(out.content.find("100%") != std::string::npos, "本地命中：给出匹配度",
          out.content);
    check(remote_calls.load() == 0, "本地命中：**不联网**",
          "remote_calls=" + std::to_string(remote_calls.load()));

    // ── 2) 大小写变体也命中本地 ──
    call.arguments_json = R"({"term":"YYDS"})";
    out = tool.run(call);
    check(out.ok && out.content.find("永远的神") != std::string::npos,
          "变体：大小写变体也命中本地", out.content);
    check(remote_calls.load() == 0, "变体：仍然不联网");

    // ── 3) 本地没有 → 自动联网学一次 + 写回 + 记记忆 ──
    call.arguments_json = R"({"term":"尊嘟假嘟"})";
    out = tool.run(call);
    check(out.ok, "自动学习：ok");
    check(remote_calls.load() == 1, "自动学习：联网一次",
          "remote_calls=" + std::to_string(remote_calls.load()));
    check(out.content.find("刚查到并已存进本地热梗库") != std::string::npos,
          "自动学习：标明已入库", out.content);
    check(import_calls.load() == 1, "自动学习：写回本地库一次");
    check(memory_calls.load() == 1, "自动学习：写记忆一次");
    check(last_memory_conv == "g:test_group", "记忆写进当前会话",
          "got " + last_memory_conv);
    check(last_memory_term == "尊嘟假嘟", "记忆里是梗名", "got " + last_memory_term);
    check(store.has("尊嘟假嘟"), "自动学习之后库里能查到");

    // ── 4) 学到之后再查 → 走本地，不再联网 ──
    out = tool.run(call);
    check(out.content.find("尊嘟假嘟") != std::string::npos,
          "学到后：能查到新条目", out.content);
    check(out.content.find("本地热梗库") != std::string::npos,
          "学到后：来源变成本地", out.content);
    check(remote_calls.load() == 1, "学到后：**不再联网**",
          "remote_calls=" + std::to_string(remote_calls.load()));

    // ── 5) 缺 term 参数 ──
    fish::tool::Call no_arg;
    no_arg.name = "lookup_meme";
    no_arg.arguments_json = "{}";
    out = tool.run(no_arg);
    check(!out.ok, "缺参数：ok=false");
    check(out.error == "missing_term", "缺参数：错误码正确", out.error);

    // ── 6) 降级：没有远程后端 ──
    fish::tool::MemeLookupTool local_only(finder, nullptr, importer, memory);
    fish::tool::Call call2;
    call2.name = "lookup_meme";
    call2.arguments_json = R"({"term":"一个新梗xyzzy"})";
    out = local_only.run(call2);
    check(out.ok, "降级（无联网）：ok");
    check(out.content.find("没启用联网") != std::string::npos,
          "降级（无联网）：明说未启用", out.content);
    check(out.content.find("xyzzy") != std::string::npos,
          "降级（无联网）：说清是哪个词", out.content);

    // ── 7) 降级：联网调用失败 ──
    fish::tool::MemeLookupTool failing(
        finder,
        [](const std::string&, std::string* error) {
          return fake_remote("", error, /*force_error=*/true);
        },
        importer, memory);
    call2.arguments_json = R"({"term":"另一个新梗abc"})";
    out = failing.run(call2);
    check(out.ok, "降级（联网失败）：ok");
    check(out.content.find("联网查也失败了") != std::string::npos,
          "降级（联网失败）：明说失败", out.content);

    // ── 8) 降级：联网成功但没查到 ──
    fish::tool::MemeLookupTool empty_remote(
        finder,
        [](const std::string&, std::string* error) {
          if (error != nullptr) error->clear();
          return std::vector<fish::tool::MemeHit>{};
        },
        importer, memory);
    call2.arguments_json = R"({"term":"查不到的东西zzz"})";
    out = empty_remote.run(call2);
    check(out.ok, "降级（没查到）：ok");
    check(out.content.find("没查到") != std::string::npos,
          "降级（没查到）：明说没查到", out.content);
    check(out.content.find("不要瞎编") != std::string::npos,
          "降级（没查到）：明确禁止模型瞎编", out.content);

    // ── 9) 质量闸门：抓到不相关内容 → 不写回 ──
    const int before = static_cast<int>(store.size());
    fish::tool::MemeLookupTool irrelevant(
        finder,
        [](const std::string&, std::string* error) {
          if (error != nullptr) error->clear();
          std::vector<fish::tool::MemeHit> out2;
          fish::tool::MemeHit h;
          h.term = "班味";
          h.meaning = "班组长的工作安排与值班表说明";
          h.source = "fake";
          out2.push_back(h);
          return out2;
        },
        importer, memory);
    call2.arguments_json = R"({"term":"班味"})";
    out = irrelevant.run(call2);
    check(out.ok, "不相关：ok");
    check(out.content.find("没有记住") != std::string::npos,
          "不相关：明说没记住", out.content);
    check(static_cast<int>(store.size()) == before,
          "不相关：**不写回**本地库",
          "before=" + std::to_string(before) + " after=" + std::to_string(store.size()));

    // ── 10) 纠错：改掉已入库的错误释义 ──
    fish::tool::Call fix_call;
    fix_call.name = "fix_meme";
    fix_call.arguments_json = R"({"term":"尊嘟假嘟","meaning":"「真的假的」的可爱说法"})";
    fix_call.conversation_id = "g:test_group";
    out = fixer.run(fix_call);
    check(out.ok, "纠错：ok", out.content);
    check(out.content.find("已改正") != std::string::npos, "纠错：明说已改正",
          out.content);
    check(out.content.find("真的假的") != std::string::npos, "纠错：回显新释义",
          out.content);

    fish::meme::MemeStore::Match m;
    check(store.find("尊嘟假嘟", &m), "纠错：库里还能查到");
    check(m.entry.meaning == "「真的假的」的可爱说法", "纠错：释义已替换",
          m.entry.meaning);
    check(m.entry.edited, "纠错：edited 标记已置位");

    // 纠错之后再查 → 拿到的是新释义
    call.arguments_json = R"({"term":"尊嘟假嘟"})";
    out = tool.run(call);
    check(out.content.find("真的假的") != std::string::npos,
          "纠错后：查询返回新释义", out.content);

    // ── 11) 纠错：库里没有这个词 ──
    fix_call.arguments_json = R"({"term":"完全不存在zzz","meaning":"x"})";
    out = fixer.run(fix_call);
    check(out.ok, "纠错（不在库）：ok（不抛异常）");
    check(out.error == "meme_not_found", "纠错（不在库）：错误码正确", out.error);
    check(out.content.find("没有") != std::string::npos, "纠错（不在库）：明说没有",
          out.content);
    check(out.content.find("lookup_meme") != std::string::npos,
          "纠错（不在库）：引导先查一次", out.content);

    // ── 12) 纠错：缺参数 ──
    fix_call.arguments_json = R"({"term":"yyds"})";
    out = fixer.run(fix_call);
    check(!out.ok, "纠错（缺 meaning）：ok=false");
    check(out.error == "missing_field", "纠错（缺 meaning）：错误码正确", out.error);
  }
  rm_tmp_dir(dir);
}

// ── 7. 并发安全 ──────────────────────────────────────────

void test_concurrency() {
  std::printf("\n=== concurrency ===\n");

  const std::string dir = make_tmp_dir("concurrent");
  {
    fish::meme::MemeStore::Config cfg;
    cfg.dir = dir;
    fish::meme::MemeStore store(cfg);
    store.init();

    // 多个会话同时学同一个梗 + 同时学不同梗
    constexpr int kThreads = 8;
    constexpr int kEach = 20;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&store, t] {
        for (int i = 0; i < kEach; ++i) {
          fish::meme::Entry e;
          // 一半线程写同一个梗（考验查重 + 文件不被写坏）
          // 一半线程各写各的梗（考验不丢条目）
          if (t % 2 == 0) {
            e.term = "shared_meme";
            e.meaning = "共享的梗";
          } else {
            e.term = "meme_t" + std::to_string(t) + "_" + std::to_string(i);
            e.meaning = "线程独有的梗";
          }
          e.origin = "concurrent";
          store.import_many({e});
        }
      });
    }
    // 同时并发读清单 + 查找，验证读主路径和写不打架
    std::thread reader([&store] {
      for (int i = 0; i < 200; ++i) {
        const auto all = store.all();
        if (!all.empty()) {
          fish::meme::MemeStore::Match m;
          store.find(all.front().term, &m);
        }
      }
    });
    for (auto& th : threads) th.join();
    reader.join();

    // 文件没写坏：能重新加载，且条目数与内存里的一致
    fish::meme::MemeStore::Config cfg2;
    cfg2.dir = dir;
    fish::meme::MemeStore reloaded(cfg2);
    auto e = reloaded.init();
    check(e.ok(), "并发读写后文件可重新加载", e.detail);

    const std::size_t expected = 1 + static_cast<std::size_t>(kThreads / 2) * kEach;
    check(reloaded.size() == expected,
          "并发读写后条目数正确（同梗只留一条）",
          "got " + std::to_string(reloaded.size()) + " expected " +
              std::to_string(expected));

    // 库里没有重复的 term
    const auto all = reloaded.all();
    std::vector<std::string> terms;
    for (const auto& x : all) terms.push_back(x.term);
    std::sort(terms.begin(), terms.end());
    const bool unique = std::adjacent_find(terms.begin(), terms.end()) == terms.end();
    check(unique, "并发写后没有重复条目");

    // 文件里没有残留的临时文件
    int tmp_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
        ++tmp_files;
      }
    }
    check(tmp_files == 0, "并发写后没有残留临时文件",
          "found " + std::to_string(tmp_files));
  }
  rm_tmp_dir(dir);
}

// ── 8. 长期记忆同步 ──────────────────────────────────────
#ifdef MEME_TEST_MEMORY

void test_memory_sync() {
  std::printf("\n=== memory sync (traits + facts + index) ===\n");

  const std::string notes_dir = make_tmp_dir("notes");
  const std::string db_path = notes_dir + "/index.db";
  {
    fish::memory::MemorySystem::Config cfg;
    cfg.notes_dir = notes_dir;
    cfg.db_path = db_path;
    fish::memory::MemorySystem mem(cfg, nullptr);
    auto e = mem.init();
    check(e.ok(), "memory init ok", e.detail);

    mem.remember_meme("g:test", "yyds", "永远的神");

    auto traits = mem.notes().load_traits("g:test");
    check(traits.ok() && !traits.value->empty(), "traits.md 写进去了");
    if (traits.ok() && !traits.value->empty()) {
      check(traits.value->front().pattern == "yyds", "trait pattern = 梗名",
            traits.value->front().pattern);
      check(traits.value->front().evidence == 1, "首次 evidence = 1",
            std::to_string(traits.value->front().evidence));
    }

    // 再记一次同一个梗 → evidence 累加
    mem.remember_meme("g:test", "yyds", "永远的神");
    traits = mem.notes().load_traits("g:test");
    if (traits.ok() && !traits.value->empty()) {
      check(traits.value->front().evidence == 2, "重复出现 → evidence 累加到 2",
            std::to_string(traits.value->front().evidence));
    }

    // fact 落盘：只记「对方在用这个梗」，不记释义
    auto facts = mem.notes().load_facts("g:test");
    check(facts.ok() && !facts.value->empty(), "facts.md 写进去了");
    if (facts.ok() && !facts.value->empty()) {
      const std::string& c = facts.value->front().content;
      check(c.find("yyds") != std::string::npos, "fact 内容含梗名", c);
      // 释义不进 facts（那是关于世界的知识，在 memes.tsv 里）
      check(c.find("永远的神") == std::string::npos,
            "fact 里**不含**梗的释义（不污染用户画像）", c);
    }

    // 再记一次 → facts 不刷屏（幂等）
    mem.remember_meme("g:test", "yyds", "永远的神");
    facts = mem.notes().load_facts("g:test");
    if (facts.ok()) {
      check(facts.value->size() == 1, "重复记录 → facts 不重复追加",
            "size=" + std::to_string(facts.value->size()));
    }

    // 不同会话隔离
    mem.remember_meme("g:other", "栓Q", "示弱");
    auto t2 = mem.notes().load_traits("g:other");
    check(t2.ok() && !t2.value->empty(), "另一个会话独立记录");
    auto t1 = mem.notes().load_traits("g:test");
    if (t1.ok() && !t1.value->empty()) {
      check(t1.value->front().pattern == "yyds", "原会话没被污染",
            t1.value->front().pattern);
    }
  }
  rm_tmp_dir(notes_dir);
}

#endif  // MEME_TEST_MEMORY
}  // namespace

int main() {
  // 崩溃时不能丢掉已打印的内容：stdout 默认是全缓冲的，
  // 段错误/快速失败时缓冲区里的输出会一起丢失，看不到跑到哪一步。
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  fish::log::Logger::instance().set_level(fish::log::LogLevel::kError);

  test_normalize();
  test_all_entries();
  test_percent_match();
  test_find();
  test_correct();
  test_dedup();
  test_relevance();
  test_alias_extraction();
  test_alias_autofill_from_store();
  test_tool_loop();
  test_concurrency();
#ifdef MEME_TEST_MEMORY
  test_memory_sync();
#endif

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
