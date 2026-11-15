// tests/probe_meme.cpp
// 热梗学习闭环的**纯逻辑**验证程序。
//
// 覆盖：
//   1. 规范化匹配（大小写 / 全角 / 别名 / 查询串包含别名）
//   2. 查重逻辑（同名 / 同别名不重复写）
//   3. 相关性校验（不相关文本不写回）
//   4. 写入并发安全（多线程同时学同一个梗 → 文件不坏、条目不重复）
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

// ── 1. 规范化匹配 ────────────────────────────────────────

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

void test_term_candidate() {
  std::printf("\n=== term_candidate ===\n");

  check(fish::meme::term_candidate("yyds") == "yyds", "纯拉丁：原样");
  check(fish::meme::term_candidate("YYDS") == "YYDS", "纯拉丁：保留原写法");
  check(fish::meme::term_candidate("这个 yyds 是什么意思") == "yyds",
        "带问法的拉丁梗 → 取出 yyds");
  check(fish::meme::term_candidate("尊嘟假嘟") == "尊嘟假嘟", "纯中文：取整段");
  check(fish::meme::term_candidate("尊嘟假嘟 是什么意思") == "尊嘟假嘟",
        "纯中文带问法 → 取第一段");
  check(fish::meme::term_candidate("  ") == "", "空串 → 空");
}

void test_local_search() {
  std::printf("\n=== local search (normalized + aliases) ===\n");

  const std::string dir = make_tmp_dir("search");
  {
    fish::meme::MemeStore::Config cfg;
    cfg.dir = dir;
    fish::meme::MemeStore store(cfg);
    auto e = store.init();
    check(e.ok(), "init ok", e.detail);

    fish::meme::Entry entry;
    entry.term = "yyds";
    entry.meaning = "永远的神，网络用语里表示非常厉害、非常牛";
    entry.usage = "「yyds 太强了」";
    entry.aliases = {"YYDS", "永远的神", "y y d s"};
    entry.origin = "manual";
    store.upsert(entry);

    fish::meme::Entry entry2;
    entry2.term = "栓Q";
    entry2.meaning = "栓头 emoji 谐音，示弱、委屈、可爱";
    entry2.aliases = {"拴Q"};
    store.upsert(entry2);

    // 大小写
    check(!store.search("YYDS").empty(), "查 YYDS 命中 yyds");
    // 全角
    check(!store.search("ＹＹＤＳ").empty(), "查 ＹＹＤＳ（全角）命中 yyds");
    // 别名
    check(!store.search("永远的神").empty(), "查别名命中");
    // 查询串包含别名
    check(!store.search("这个 永远的神 什么意思").empty(), "查询串含别名命中");
    // 变体（拴Q ↔ 栓Q）
    check(!store.search("拴Q").empty(), "变体 拴Q 命中 栓Q");
    // 无命中
    check(store.search("完全不存在的一个词xyzzy").empty(), "无关词不命中");
    // 别名查重
    check(store.has("YYDS"), "has：别名也算已收录");
    check(store.has("拴Q"), "has：变体也算已收录");
    check(!store.has("zzz不存在的词"), "has：无关词不算已收录");
  }
  rm_tmp_dir(dir);
}

// ── 2. 查重 ──────────────────────────────────────────────

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
    // （新条目的 term 是新词，但它的别名等于库里已有的 term）
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
  }
  rm_tmp_dir(dir);
}

// ── 3. 相关性校验 ────────────────────────────────────────

void test_relevance() {
  std::printf("\n=== relevance gate ===\n");

  // 相关的
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
        "不相关：搜到的是同名别的东西");
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

// 存量 24 条数据的别名自动补全：加载后查别名能命中
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

    // 别名被自动抽出来 → 查别名能命中
    check(!store.search("蓝色大肥鱼").empty(), "自动抽出的别名能命中");
    check(!store.search("吃白饭的大肥鱼").empty(), "自动抽出的别名2能命中");
    // 原梗名当然也能命中
    check(!store.search("大肥鱼").empty(), "原梗名能命中");
    // 没有别名关系的词不命中
    check(store.search("蓝色大鲸鱼").empty(), "非别名不命中");
  }
  rm_tmp_dir(dir);
}

// ── 4. 工具闭环（假的远程响应，不真联网）──────────────────

// 假远程搜索：按 query 返回预置的响应体
std::vector<fish::tool::MemeLookupTool::Hit> fake_remote(
    const std::string& query, std::string* error, bool force_error = false) {
  std::vector<fish::tool::MemeLookupTool::Hit> out;
  if (error != nullptr) error->clear();
  if (force_error) {
    if (error != nullptr) *error = "search_401_unauthorized";
    return out;
  }
  fish::tool::MemeLookupTool::Hit h;
  h.term = query;
  h.meaning = query + " 是网络用语，意思是「永远的神」，夸人很厉害。常说「" + query + " 太强了」。";
  h.usage = "「" + query + " 太强了」";
  h.source = "fake";
  out.push_back(h);
  return out;
}

void test_tool_loop() {
  std::printf("\n=== tool: local hit / learn / degrade ===\n");

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

    auto local = [&store](const std::string& q) {
      std::vector<fish::tool::MemeLookupTool::Hit> out;
      for (const auto& e : store.search(q)) {
        out.push_back({e.term, e.meaning, e.usage, "local"});
      }
      return out;
    };
    auto remote = [&remote_calls](const std::string& q, std::string* error) {
      ++remote_calls;
      return fake_remote(q, error);
    };
    auto importer = [&store, &import_calls](
                        const std::vector<fish::tool::MemeLookupTool::Hit>& hits) {
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
                      const std::string& meaning) {
      ++memory_calls;
      last_memory_conv = conv;
      last_memory_term = term;
    };

    fish::tool::MemeLookupTool tool(local, remote, importer, memory);

    // ── 本地命中 → 不联网 ──
    fish::meme::Entry seed;
    seed.term = "yyds";
    seed.meaning = "永远的神";
    store.upsert(seed);

    fish::tool::Call call;
    call.name = "lookup_meme";
    call.arguments_json = R"({"query":"yyds"})";
    call.conversation_id = "g:test_group";
    auto out = tool.run(call);
    check(out.ok, "本地命中：ok");
    check(out.content.find("本地热梗库") != std::string::npos,
          "本地命中：标明来源", out.content);
    check(remote_calls.load() == 0, "本地命中：**不联网**",
          "remote_calls=" + std::to_string(remote_calls.load()));

    // ── 本地没有 → 联网学到 + 写回 + 记记忆 ──
    call.arguments_json = R"({"query":"尊嘟假嘟"})";
    out = tool.run(call);
    check(out.ok, "联网学到：ok");
    check(remote_calls.load() == 1, "本地没有 → 联网一次",
          "remote_calls=" + std::to_string(remote_calls.load()));
    check(out.content.find("刚查到并已记住") != std::string::npos,
          "联网学到：标明「刚查到并已记住」", out.content);
    check(import_calls.load() == 1, "联网学到：写回本地库一次");
    check(memory_calls.load() == 1, "联网学到：写记忆一次");
    check(last_memory_conv == "g:test_group", "记忆写进当前会话",
          "got " + last_memory_conv);
    check(last_memory_term == "尊嘟假嘟", "记忆里是梗名", "got " + last_memory_term);
    check(store.has("尊嘟假嘟"), "写回后库里能查到");

    // ── 下次同样的梗 → 本地命中，不再联网 ──
    out = tool.run(call);
    check(out.content.find("本地热梗库") != std::string::npos,
          "第二次：本地命中", out.content);
    check(remote_calls.load() == 1, "第二次：**不再联网**",
          "remote_calls=" + std::to_string(remote_calls.load()));

    // ── 降级：没有远程后端 ──
    fish::tool::MemeLookupTool local_only(local, nullptr, importer, memory);
    fish::tool::Call call2;
    call2.name = "lookup_meme";
    call2.arguments_json = R"({"query":"一个新梗xyzzy"})";
    out = local_only.run(call2);
    check(out.ok, "降级（无联网）：ok");
    check(out.content.find("未启用联网检索") != std::string::npos,
          "降级（无联网）：明说未启用", out.content);
    check(out.content.find("xyzzy") != std::string::npos,
          "降级（无联网）：说清是哪个词", out.content);

    // ── 降级：联网调用失败 ──
    fish::tool::MemeLookupTool failing(
        local,
        [](const std::string&, std::string* error) {
          return fake_remote("", error, /*force_error=*/true);
        },
        importer, memory);
    call2.arguments_json = R"({"query":"另一个新梗abc"})";
    out = failing.run(call2);
    check(out.ok, "降级（联网失败）：ok");
    check(out.content.find("联网检索也失败了") != std::string::npos,
          "降级（联网失败）：明说失败", out.content);

    // ── 降级：联网成功但没搜到 ──
    fish::tool::MemeLookupTool empty_remote(
        local,
        [](const std::string&, std::string* error) {
          if (error != nullptr) error->clear();
          return std::vector<fish::tool::MemeLookupTool::Hit>{};
        },
        importer, memory);
    call2.arguments_json = R"({"query":"搜不到的东西zzz"})";
    out = empty_remote.run(call2);
    check(out.ok, "降级（没搜到）：ok");
    check(out.content.find("没搜到") != std::string::npos,
          "降级（没搜到）：明说没搜到", out.content);
    check(out.content.find("不要瞎编") != std::string::npos,
          "降级（没搜到）：明确禁止模型瞎编", out.content);

    // ── 质量闸门：抓到不相关内容 → 不写回 ──
    const int before = static_cast<int>(store.size());
    fish::tool::MemeLookupTool irrelevant(
        local,
        [](const std::string&, std::string* error) {
          if (error != nullptr) error->clear();
          std::vector<fish::tool::MemeLookupTool::Hit> out;
          fish::tool::MemeLookupTool::Hit h;
          h.term = "班味";
          h.meaning = "班组长的工作安排与值班表说明";
          h.source = "fake";
          out.push_back(h);
          return out;
        },
        importer, memory);
    call2.arguments_json = R"({"query":"班味"})";
    out = irrelevant.run(call2);
    check(out.ok, "不相关：ok");
    check(out.content.find("没有记住") != std::string::npos,
          "不相关：明说没记住", out.content);
    check(static_cast<int>(store.size()) == before,
          "不相关：**不写回**本地库",
          "before=" + std::to_string(before) + " after=" + std::to_string(store.size()));
    check(memory_calls.load() == 1, "不相关：不写记忆",
          "memory_calls=" + std::to_string(memory_calls.load()));
  }
  rm_tmp_dir(dir);
}

// ── 5. 并发安全 ──────────────────────────────────────────

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
    for (auto& th : threads) th.join();

    // 文件没写坏：能重新加载，且行数与内存里的一致
    fish::meme::MemeStore::Config cfg2;
    cfg2.dir = dir;
    fish::meme::MemeStore reloaded(cfg2);
    auto e = reloaded.init();
    check(e.ok(), "并发写后文件可重新加载", e.detail);

    const std::size_t expected = 1 + static_cast<std::size_t>(kThreads / 2) * kEach;
    check(reloaded.size() == expected,
          "并发写后条目数正确（同梗只留一条）",
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

// ── 6. 长期记忆同步 ──────────────────────────────────────
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
  test_term_candidate();
  test_local_search();
  test_alias_extraction();
  test_alias_autofill_from_store();
  test_dedup();
  test_relevance();
  test_tool_loop();
  test_concurrency();
#ifdef MEME_TEST_MEMORY
  test_memory_sync();
#endif

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
