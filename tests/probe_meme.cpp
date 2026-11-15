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

void test_detail() {
  std::printf("\n=== detail(index) ===\n");

  const std::string dir = make_tmp_dir("detail");
  {
    fish::meme::MemeStore::Config cfg;
    cfg.dir = dir;
    fish::meme::MemeStore store(cfg);
    store.init();

    // added_at 不同：验证「新的在前」
    fish::meme::Entry old_one;
    old_one.term = "old_one";
    old_one.meaning = "旧的一条";
    old_one.added_at = "2026-01-01";
    store.upsert(old_one);

    fish::meme::Entry new_one;
    new_one.term = "new_one";
    new_one.meaning = "新的一条";
    new_one.added_at = "2026-11-15";
    store.upsert(new_one);

    const auto all = store.all();
    check(all.size() == 2, "两条都在", "got " + std::to_string(all.size()));
    check(!all.empty() && all[0].term == "new_one", "新的在前（added_at 倒序）",
          all.empty() ? "empty" : all[0].term);

    // index 0 = 最新的那条
    fish::meme::Entry got;
    check(store.detail(0, &got), "index 0 取得到");
    check(got.term == all[0].term, "index 0 拿到清单第 0 条", got.term);
    check(got.meaning == all[0].meaning, "释义一并给出", got.meaning);
    check(got.usage == all[0].usage, "用法一并给出", got.usage);

    // hits 只在取详情时 +1（翻清单不计数）
    const auto after_one = store.all();
    check(after_one[0].hits >= 1, "取详情后 hits 累加",
          "hits=" + std::to_string(after_one[0].hits));

    // index 1 = 旧的那条
    fish::meme::Entry got2;
    check(store.detail(1, &got2), "index 1 取得到");
    check(got2.term == "old_one", "index 1 拿到第 1 条", got2.term);

    // 越界
    fish::meme::Entry oob;
    check(!store.detail(2, &oob), "index == size → 越界返回 false");
    check(!store.detail(999, &oob), "index 远大于 size → 越界返回 false");

    // 空库
    fish::meme::MemeStore::Config cfg2;
    cfg2.dir = dir + "/empty";
    fish::meme::MemeStore empty_store(cfg2);
    empty_store.init();
    check(!empty_store.detail(0, &oob), "空库 detail(0) → false");
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

// ── 6. 工具闭环（假的远程响应，不真联网）──────────────────

// 假远程学梗：按 term 返回预置的响应体
std::vector<fish::tool::MemeLookupTool::Hit> fake_remote(
    const std::string& term, std::string* error, bool force_error = false) {
  std::vector<fish::tool::MemeLookupTool::Hit> out;
  if (error != nullptr) error->clear();
  if (force_error) {
    if (error != nullptr) *error = "search_401_unauthorized";
    return out;
  }
  fish::tool::MemeLookupTool::Hit h;
  h.term = term;
  h.meaning = term + " 是网络用语，意思是「永远的神」，夸人很厉害。常说「" + term + " 太强了」。";
  h.usage = "「" + term + " 太强了」";
  h.source = "fake";
  out.push_back(h);
  return out;
}

void test_tool_loop() {
  std::printf("\n=== tool: 清单 / 取详情 / learn / 降级 ===\n");

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

    // 与 main.cpp 的装配一一对应：清单走 all()，详情走 detail()，不检索。
    auto lister = [&store]() {
      std::vector<fish::tool::MemeLookupTool::Hit> out;
      for (const auto& e : store.all()) {
        fish::tool::MemeLookupTool::Hit h;
        h.term = e.term;
        h.meaning = e.meaning;
        h.usage = e.usage;
        h.source = "local";
        for (std::size_t i = 0; i < e.aliases.size(); ++i) {
          if (i > 0) h.aliases += "、";
          h.aliases += e.aliases[i];
        }
        out.push_back(std::move(h));
      }
      return out;
    };
    auto detailer = [&store](long long index, fish::tool::MemeLookupTool::Hit* out) {
      if (out == nullptr || index < 0) return false;
      fish::meme::Entry e;
      if (!store.detail(static_cast<std::size_t>(index), &e)) return false;
      out->term = e.term;
      out->meaning = e.meaning;
      out->usage = e.usage;
      out->source = "local";
      return true;
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
                      const std::string&) {
      ++memory_calls;
      last_memory_conv = conv;
      last_memory_term = term;
    };

    fish::tool::MemeLookupTool tool(lister, detailer, remote, importer, memory);

    // ── 1) 空参数 → 全部清单，不联网 ──
    fish::meme::Entry seed;
    seed.term = "yyds";
    seed.meaning = "永远的神";
    seed.aliases = {"YYDS"};
    store.upsert(seed);

    fish::meme::Entry seed2;
    seed2.term = "栓Q";
    seed2.meaning = "示弱、委屈";
    store.upsert(seed2);

    fish::tool::Call call;
    call.name = "lookup_meme";
    call.arguments_json = R"({})";
    call.conversation_id = "g:test_group";
    auto out = tool.run(call);
    check(out.ok, "清单：ok");
    check(out.content.find("清单") != std::string::npos, "清单：说明这是清单",
          out.content);
    check(out.content.find("共 2 条") != std::string::npos, "清单：报出总条数",
          out.content);
    check(out.content.find("[0]") != std::string::npos, "清单：带序号 [0]");
    check(out.content.find("[1]") != std::string::npos, "清单：带序号 [1]");
    check(out.content.find("yyds") != std::string::npos, "清单：含梗名",
          out.content);
    check(out.content.find("永远的神") != std::string::npos, "清单：含一句话释义");
    check(out.content.find("也叫") != std::string::npos, "清单：展示别名");
    check(remote_calls.load() == 0, "清单：**不联网**",
          "remote_calls=" + std::to_string(remote_calls.load()));

    // ── 2) 带 index → 取详情 ──
    // 先算出 yyds 在清单里的序号（排序是新的在前 + term 字典序，不假定顺序）
    long long idx_yyds = -1;
    {
      const auto all = store.all();
      for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].term == "yyds") idx_yyds = static_cast<long long>(i);
      }
    }
    check(idx_yyds >= 0, "yyds 在清单里找得到序号",
          "idx=" + std::to_string(idx_yyds));

    fish::tool::Call detail_call;
    detail_call.name = "lookup_meme";
    detail_call.arguments_json = "{\"index\":" + std::to_string(idx_yyds) + "}";
    detail_call.conversation_id = "g:test_group";
    out = tool.run(detail_call);
    check(out.ok, "详情：ok");
    check(out.content.find("永远的神") != std::string::npos, "详情：拿到释义",
          out.content);
    check(out.content.find("本地热梗库") != std::string::npos, "详情：标明来源",
          out.content);
    check(remote_calls.load() == 0, "详情：**不联网**",
          "remote_calls=" + std::to_string(remote_calls.load()));

    // 取详情会累加 hits
    {
      const auto all = store.all();
      for (const auto& x : all) {
        if (x.term == "yyds") {
          check(x.hits == 1, "取详情后 hits == 1", "hits=" + std::to_string(x.hits));
        }
      }
    }

    // ── 3) index 越界 → 明说序号不对，不静默退回第 0 条 ──
    fish::tool::Call oob_call;
    oob_call.name = "lookup_meme";
    oob_call.arguments_json = R"({"index":99})";
    out = tool.run(oob_call);
    check(out.ok, "越界：ok（不抛异常）");
    check(out.content.find("超出清单范围") != std::string::npos, "越界：明说超出范围",
          out.content);
    check(out.content.find("共 2 条") != std::string::npos, "越界：告知实际条数");
    check(out.content.find("永远的神") == std::string::npos,
          "越界：**不**把第 0 条当成模型挑的那条返回");

    // ── 4) learn → 联网学到 + 写回 + 记记忆 ──
    fish::tool::Call learn_call;
    learn_call.name = "lookup_meme";
    learn_call.arguments_json = R"({"learn":"尊嘟假嘟"})";
    learn_call.conversation_id = "g:test_group";
    out = tool.run(learn_call);
    check(out.ok, "learn：ok");
    check(remote_calls.load() == 1, "learn：联网一次",
          "remote_calls=" + std::to_string(remote_calls.load()));
    check(out.content.find("刚查到并已记住") != std::string::npos,
          "learn：标明「刚查到并已记住」", out.content);
    check(import_calls.load() == 1, "learn：写回本地库一次");
    check(memory_calls.load() == 1, "learn：写记忆一次");
    check(last_memory_conv == "g:test_group", "记忆写进当前会话",
          "got " + last_memory_conv);
    check(last_memory_term == "尊嘟假嘟", "记忆里是梗名", "got " + last_memory_term);
    check(store.has("尊嘟假嘟"), "learn 之后库里能查到");

    // ── 5) 学到之后 → 清单里就有它，不再联网 ──
    out = tool.run(call);
    check(out.content.find("尊嘟假嘟") != std::string::npos,
          "学到后：清单里出现新条目", out.content);
    check(out.content.find("共 3 条") != std::string::npos, "学到后：清单条数 +1",
          out.content);
    check(remote_calls.load() == 1, "学到后：清单**不再联网**",
          "remote_calls=" + std::to_string(remote_calls.load()));

    // ── 6) 降级：没有远程后端 ──
    fish::tool::MemeLookupTool local_only(lister, detailer, nullptr, importer, memory);
    fish::tool::Call call2;
    call2.name = "lookup_meme";
    call2.arguments_json = R"({"learn":"一个新梗xyzzy"})";
    out = local_only.run(call2);
    check(out.ok, "降级（无联网）：ok");
    check(out.content.find("未启用联网") != std::string::npos,
          "降级（无联网）：明说未启用", out.content);
    check(out.content.find("xyzzy") != std::string::npos,
          "降级（无联网）：说清是哪个词", out.content);

    // ── 7) 降级：联网调用失败 ──
    fish::tool::MemeLookupTool failing(
        lister, detailer,
        [](const std::string&, std::string* error) {
          return fake_remote("", error, /*force_error=*/true);
        },
        importer, memory);
    call2.arguments_json = R"({"learn":"另一个新梗abc"})";
    out = failing.run(call2);
    check(out.ok, "降级（联网失败）：ok");
    check(out.content.find("联网检索失败") != std::string::npos,
          "降级（联网失败）：明说失败", out.content);

    // ── 8) 降级：联网成功但没查到 ──
    fish::tool::MemeLookupTool empty_remote(
        lister, detailer,
        [](const std::string&, std::string* error) {
          if (error != nullptr) error->clear();
          return std::vector<fish::tool::MemeLookupTool::Hit>{};
        },
        importer, memory);
    call2.arguments_json = R"({"learn":"查不到的东西zzz"})";
    out = empty_remote.run(call2);
    check(out.ok, "降级（没查到）：ok");
    check(out.content.find("没查到") != std::string::npos,
          "降级（没查到）：明说没查到", out.content);
    check(out.content.find("不要瞎编") != std::string::npos,
          "降级（没查到）：明确禁止模型瞎编", out.content);

    // ── 9) 质量闸门：抓到不相关内容 → 不写回 ──
    const int before = static_cast<int>(store.size());
    fish::tool::MemeLookupTool irrelevant(
        lister, detailer,
        [](const std::string&, std::string* error) {
          if (error != nullptr) error->clear();
          std::vector<fish::tool::MemeLookupTool::Hit> out2;
          fish::tool::MemeLookupTool::Hit h;
          h.term = "班味";
          h.meaning = "班组长的工作安排与值班表说明";
          h.source = "fake";
          out2.push_back(h);
          return out2;
        },
        importer, memory);
    call2.arguments_json = R"({"learn":"班味"})";
    out = irrelevant.run(call2);
    check(out.ok, "不相关：ok");
    check(out.content.find("没有记住") != std::string::npos,
          "不相关：明说没记住", out.content);
    check(static_cast<int>(store.size()) == before,
          "不相关：**不写回**本地库",
          "before=" + std::to_string(before) + " after=" + std::to_string(store.size()));
    check(memory_calls.load() == 1, "不相关：不写记忆",
          "memory_calls=" + std::to_string(memory_calls.load()));

    // ── 10) 空库时给清单 → 引导用 learn ──
    {
      fish::meme::MemeStore::Config cfg3;
      cfg3.dir = dir + "/empty2";
      fish::meme::MemeStore empty_store(cfg3);
      empty_store.init();
      auto empty_lister = [&empty_store]() {
        std::vector<fish::tool::MemeLookupTool::Hit> v;
        for (const auto& e : empty_store.all()) v.push_back({e.term, e.meaning,
                                                             e.usage, "local", ""});
        return v;
      };
      fish::tool::MemeLookupTool t2(empty_lister, detailer, remote, importer, memory);
      fish::tool::Call c3;
      c3.name = "lookup_meme";
      c3.arguments_json = "{}";
      out = t2.run(c3);
      check(out.ok && out.content.find("空的") != std::string::npos,
            "空库：明说库是空的", out.content);
      check(out.content.find("learn") != std::string::npos,
            "空库：引导模型用 learn");
    }
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
    // 同时并发读清单 + 取详情，验证读主路径和写不打架
    std::thread reader([&store] {
      for (int i = 0; i < 200; ++i) {
        const auto all = store.all();
        if (!all.empty()) {
          fish::meme::Entry e;
          store.detail(0, &e);
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
  test_detail();
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
