// src/fish.meme.cppm
// 网络热梗库：按词查、学新词、纠错。
//
// 设计：
//   - 独立存储区（data/memes/），与记忆分开：
//     热梗是「公共知识」，不随会话隔离；记忆是「私人上下文」
//   - 权威源是纯文本文件（人可读、可手改、可增删）
//   - **查找靠百分比相似度，不做子串匹配**：
//     lookup_meme 只收一个词，程序用「字符二元组 Dice 相似度」算百分比，
//     达到阈值才算命中。命中直接给库里的释义；不命中就自动联网学一次并写回。
//     为什么不是子串：子串是 0/1 判断，「拴Q」查不到「栓Q」的条目，
//     而「这个」「意思」这种片段又会命中半张表。百分比既能容忍写法差异，
//     又能把置信度量出来——没到阈值就是没命中，交给联网去查。
//
// 为什么需要本地库：
//   模型训练数据有截止日期，新梗它不知道。若每次都联网查，既慢又贵。
//   所以本地建库：学到一次就记住，之后零成本。
module;

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

export module fish.meme;

import fish.core;
import fish.log;

export namespace fish::meme {

// 一条热梗
struct Entry {
  std::string term;        // 梗本身
  std::string meaning;     // 含义解释
  std::string usage;       // 用法示例
  std::string origin;      // 来源（可选）
  std::string added_at;    // yyyy-mm-dd
  // 别名 / 变体：同一个梗的其它写法。
  //
  // 两个用途：
  //   1. 查重 / 匹配：别名撞别名也算同一个梗
  //   2. 命中时一并展示：「梗名（也叫 别名1、别名2）」
  std::vector<std::string> aliases;
  // 被取详情次数（命中时 +1）。
  int hits{0};
  // 是否被人工 / 模型纠正过（fix_meme 写入后置 true）。
  //
  // 为什么需要：自动联网学来的释义质量参差，纠正过的才是可信的。
  //   再次联网学到同一条时，不许把纠正过的释义覆盖回去。
  bool edited{false};

  // 这条条目有没有被这个名字命中（名字与 term 或任一 alias 匹配）。
  [[nodiscard]] bool matches(const std::string& name, int* percent_out = nullptr) const;
};

// 规范化：全角→半角、ASCII 小写、去掉所有空白。
std::string normalize_text(const std::string& s);

// 按**字符**数截断（不是字节）。
//
// 为什么必须按字符：中文一个字 3 字节，按字节切会把最后一个字切成半个，
// 产生非法 UTF-8。这类损坏会一路写进 memes.tsv，之后整个文件都读不出来
// （实测：一条释义被按字节截断后，库里出现半个汉字，整个 TSV 解析失败）。
std::string truncate_chars(const std::string& s, std::size_t max_chars);

// 名字相似度（0~100 的百分比）。
//
// 算法：字符二元组 Dice 系数。
//   1. 两侧都规范化（全角→半角、小写、去空白、去标点）
//   2. 切成字符（不是字节，中文一个字算一个）
//   3. 相邻两字符组成二元组，取多重集合
//   4. Dice = 2 * |交集| / (|A| + |B|)
//
// 为什么用二元组而不是编辑距离：编辑距离对「yyds」和「yyds是什么意思」
// 会算出很高的相似度（只多了 5 个字，距离 5），而二元组把多出来的字
// 也算进分母，得分会明显掉下来，正好符合我们要的「不像同一个词」。
//
// 完全相同 → 100。任一侧为空 → 0。
int percent_match(const std::string& a, const std::string& b);

// 判定阈值：相似度 >= 这个百分比才算「库里就是这个梗」。
//
// 定在 50 的理由：实测「yyds」对「yyds是什么意思」是 55%，
// 对「abc」是 0%；而「什么意思」对「yyds是什么意思」也是 55%——
// 所以命中后还要**按分数排序**取最高的那条，阈值只负责「够不够格」。
// 没到阈值 → 当成库里没有，交给联网去学，而不是拿一条不相干的糊弄过去。
inline constexpr int kMatchThreshold = 50;

// 相关性闸门：抓回来的正文是否**确实在讲这个梗**。
//
// 【与「关键词匹配」的区别，务必分清】
//   这里判的是「**联网抓回来的第三方内容**跟我们要学的那个梗有没有关系」，
//   是数据入库前的质量闸门：搜「班味」搜到班组长的值班表，这种内容
//   写进库会污染本地数据，越查越烂。
//   它**不是**「判断用户这句话在问什么」，也不参与模型侧的选梗。
//
// term_out 出参：正文里实际出现的那个梗名（写回时用它当条目的 term）。
// 相关时返回 true。
bool is_relevant(const std::string& query, const std::string& text,
                 std::string* term_out = nullptr);

// 从释义里自动抽取别名 / 变体。
//
// 为什么需要：存量热梗的别名是**混在释义的自然语言里**的，
// 抽出来当别名，匹配时就能命中。
// 这是**数据增强**，不是匹配。
//
// 识别的是**结构化的引导词**（也叫 / 又叫 / 也称 / 又称 / 俗称 / 别称），
// 不是关键词黑名单：它不判断「这条释义在讲什么」，
// 只看有没有「别名引导词 + 一串并列项」这个句式。
std::vector<std::string> extract_aliases(const std::string& meaning);

// 相关性闸门（加强版）：梗名出现在文本里 **且** 这段文本像是在解释它。
//
// 同上：这是**抓取结果入库前的质量闸门**，不是判断用户意图。
bool is_relevant_with_context(const std::string& query, const std::string& text,
                              std::string* term_out = nullptr);

// 从正文里摘一句带引号的用法示例。找不到返回空串。
//
// 只取原文里已经存在的引号片段，不自己编。
std::string extract_usage_example(const std::string& text, const std::string& term);

class MemeStore {
 public:
  struct Config {
    std::string dir{"data/memes"};
  };

  // 一次查找的结果。
  struct Match {
    Entry entry;
    int percent{0};   // 相似度百分比
    bool exact{false};  // 是否完全同名（term 或别名一模一样）
  };

  explicit MemeStore(Config cfg);

  // 建目录 + 加载。幂等。
  fish::core::Error init();

  // 重新加载（外部手改文件后调用）
  fish::core::Error reload();

  // 是否已收录（同名或同别名都算，避免重复入库）
  [[nodiscard]] bool has(const std::string& term) const;

  // 新增/更新一条。已存在则更新内容并保留 hits。
  fish::core::Error upsert(const Entry& e);

  // 批量导入（从外部搜索结果）。
  // 查重：同名或同别名都不重复写。返回实际新增条数。
  //
  // 已经被 fix_meme 纠正过的条目**不会被覆盖**：释义以人工纠正为准，
  // 自动学来的内容只补空缺（usage / origin），不动 meaning。
  int import_many(const std::vector<Entry>& entries);

  [[nodiscard]] std::size_t size() const;

  // 全部条目（快照）。排序：added_at 倒序，同一天按 term 字典序兜底。
  std::vector<Entry> all() const;

  // 按名字找一条。
  //
  // 判据：对每条条目的所有名字（term + aliases）算 percent_match，
  // 取最高分；最高分 < kMatchThreshold 视为没找到。
  // 完全同名（100%）直接胜出，不再算别的。
  //
  // 不插兜底：找不到就是 false，调用方去联网学，不拿不相干的条目糊弄。
  bool find(const std::string& name, Match* out) const;

  // 纠正一条已入库的释义。
  //
  // found=false（含「库里没有」）时返回 kNotFound，调用方据此决定
  // 是回「库里没有」还是回「纠正失败」。
  //
  // 语义：**只改 meaning**，保留原 term / usage / origin / added_at / aliases / hits，
  // 并把 edited 置 true（此后自动学习不会覆盖它）。
  //
  // 为什么纠正只改释义而不新增条目：要纠正的东西是「释义写错了」，
  // 梗名本身没错。新增一条会造成同一个梗两条互相矛盾的记录。
  fish::core::Error correct(const std::string& term, const std::string& meaning,
                            Match* out);

 private:
  fish::core::Error load();
  // 落盘。调用方必须持有 mu_。
  fish::core::Error save_locked() const;
  [[nodiscard]] std::string file_path() const;
  // 写临时文件用的唯一名：多个会话同时学同一个梗时，
  // 共用一个 .tmp 会互相覆盖，rename 出来的文件就是坏的。
  [[nodiscard]] std::string tmp_path() const;
  // 查找实现。调用方必须持有 mu_。
  bool find_locked(const std::string& name, Match* out) const;

  Config cfg_;
  mutable std::mutex mu_;
  std::vector<Entry> items_;
};

}  // namespace fish::meme

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::meme {

namespace fs = std::filesystem;

namespace detail {

std::string today_str() {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday);
  return buf;
}

std::string trim(std::string s) {
  const auto not_space = [](unsigned char c) {
    return c != ' ' && c != '\t' && c != '\r' && c != '\n';
  };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

// ── UTF-8 基础 ──────────────────────────────────────────

// 解码一个 UTF-8 码点。len 出参是字节数；非法字节按 1 字节处理。
std::uint32_t decode_utf8(const std::string& s, std::size_t i, std::size_t& len) {
  const auto b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) {
    len = 1;
    return b0;
  }
  auto cont = [&](std::size_t k) {
    return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
  };
  if ((b0 & 0xE0) == 0xC0 && cont(1)) {
    len = 2;
    return static_cast<std::uint32_t>(b0 & 0x1F) << 6 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3F);
  }
  if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
    len = 3;
    return static_cast<std::uint32_t>(b0 & 0x0F) << 12 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + 2]) & 0x3F);
  }
  if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
    len = 4;
    return static_cast<std::uint32_t>(b0 & 0x07) << 18 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + 3]) & 0x3F);
  }
  len = 1;
  return b0;
}

// 是不是分隔符（空白 / 标点）。
bool is_separator_cp(std::uint32_t cp) {
  if (cp < 0x80) {
    // ASCII：字母数字不是分隔符，其余（空白、标点）都是
    return std::isalnum(static_cast<int>(cp)) == 0;
  }
  // CJK 标点、书名号、引号、省略号、破折号
  if (cp >= 0x3000 && cp <= 0x303F) return true;   // 。、《》「」【】…
  if (cp >= 0x2010 && cp <= 0x2027) return true;   // —— ‘’“”…
  if (cp == 0x00B7) return true;                   // · 间隔号（中文里当分隔用）
  if (cp >= 0xFF01 && cp <= 0xFF20) return true;   // 全角标点
  if (cp >= 0xFF3B && cp <= 0xFF40) return true;   // 全角标点
  if (cp >= 0xFF5B && cp <= 0xFF65) return true;   // 全角标点
  return false;
}

// 常见变体：写法不同但说的是同一个梗。
//
// 只收「实际会被混用」的字，不做通用模糊匹配——
// 模糊匹配会把不相关的词也拉进来，模型会以为自己知道了。
//
// 注意：百分比匹配本身已经能容忍一部分写法差异（拴Q ↔ 栓Q 的二元组
// 只有第一个字不同，Dice 仍然有 50%），但把它做成显式等价更可靠。
constexpr const std::pair<const char*, const char*> kVariants[] = {
    {"拴", "栓"},   // 拴Q / 栓Q
};

// 变体替换（在规范化之后做）。逐字节扫描，匹配到就整段替换。
//
// UTF-8 的续字节（0x80..0xBF）不会等于任何前导字节，
// 所以匹配点必然落在字符边界上，不会切坏汉字。
std::string apply_variants(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    bool replaced = false;
    for (const auto& [from, to] : kVariants) {
      const std::size_t flen = std::strlen(from);
      if (s.compare(i, flen, from) == 0) {
        out += to;
        i += flen;
        replaced = true;
        break;
      }
    }
    if (replaced) continue;
    out += s[i];
    ++i;
  }
  return out;
}

// 一个词的全部可匹配形式：规范化 + 变体替换。
std::vector<std::string> forms_of(const std::string& s) {
  std::vector<std::string> out;
  const std::string n = normalize_text(s);
  if (n.empty()) return out;
  out.push_back(n);
  const std::string v = apply_variants(n);
  if (v != n) out.push_back(v);
  return out;
}

// 拆成「字符符号」：规范化 + 变体替换 + 丢掉分隔符。
//
// 返回的是每个字符的 UTF-8 字节序列，不是字节。中文一个字算一个符号。
std::vector<std::string> symbols_of(const std::string& s) {
  std::vector<std::string> out;
  const std::string base = apply_variants(normalize_text(s));
  std::size_t i = 0;
  while (i < base.size()) {
    std::size_t len = 1;
    const std::uint32_t cp = decode_utf8(base, i, len);
    if (!is_separator_cp(cp)) out.push_back(base.substr(i, len));
    i += len;
  }
  return out;
}

// 字符二元组（有序对）。单字符词退化成该字符本身，保证还能参与比较。
std::vector<std::string> bigrams_of(const std::string& s) {
  const auto sym = symbols_of(s);
  std::vector<std::string> out;
  if (sym.empty()) return out;
  if (sym.size() == 1) {
    out.push_back(sym[0]);
    return out;
  }
  out.reserve(sym.size() - 1);
  for (std::size_t i = 0; i + 1 < sym.size(); ++i) {
    out.push_back(sym[i] + sym[i + 1]);
  }
  return out;
}

// 两个多元组集合的交集大小（多重集合：重复项按较少的一侧计数）。
std::size_t multiset_intersection(std::vector<std::string> a, std::vector<std::string> b) {
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  std::size_t i = 0;
  std::size_t j = 0;
  std::size_t n = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] == b[j]) {
      ++n;
      ++i;
      ++j;
    } else if (a[i] < b[j]) {
      ++i;
    } else {
      ++j;
    }
  }
  return n;
}

// 字符数（不是字节数）。中文一个字 3 字节，按字节判长度会判错。
std::size_t char_count(const std::string& s) {
  std::size_t n = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t len = 1;
    decode_utf8(s, i, len);
    i += len;
    ++n;
  }
  return n;
}

// 查重用：两个词规范化后是不是同一个梗。
bool same_term(const std::string& a, const std::string& b) {
  const auto fa = forms_of(a);
  const auto fb = forms_of(b);
  for (const auto& x : fa) {
    for (const auto& y : fb) {
      if (!x.empty() && x == y) return true;
    }
  }
  return false;
}

// 一个条目的全部名字：term + 别名。
std::vector<std::string> names_of(const Entry& e) {
  std::vector<std::string> v;
  v.push_back(e.term);
  for (const auto& a : e.aliases) v.push_back(a);
  return v;
}

}  // namespace detail

// ── 导出的自由函数 ───────────────────────────────────────

std::string normalize_text(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++i;
        continue;
      }
      out += static_cast<char>(std::tolower(c));
      ++i;
      continue;
    }
    // 全角 ASCII（U+FF01..U+FF5E）→ 半角。
    // UTF-8 编码：U+FFxx → EF BC (0x80 + (xx - 0x00))，第三字节 0x81..0xBE。
    // 半角码点 = 第三字节 - 0x60（U+FF21 '！' → 0x81-0x60 = 0x21 '!'）。
    //
    // 范围写成 0x81..0x9E 是错的：那只能覆盖到 U+FF1E，
    // 全角字母（Ｙ = U+FF39 → 第三字节 0xB9）会漏掉。
    if (c == 0xEF && i + 2 < s.size() &&
        static_cast<unsigned char>(s[i + 1]) == 0xBC) {
      const unsigned char b = static_cast<unsigned char>(s[i + 2]);
      if (b >= 0x81 && b <= 0xBE) {
        out += static_cast<char>(std::tolower(static_cast<int>(b - 0x60)));
        i += 3;
        continue;
      }
    }
    // 全角空格（U+3000）
    if (c == 0xE3 && i + 2 < s.size() &&
        static_cast<unsigned char>(s[i + 1]) == 0x80 &&
        static_cast<unsigned char>(s[i + 2]) == 0x80) {
      i += 3;
      continue;
    }
    out += s[i];
    ++i;
  }
  return out;
}

int percent_match(const std::string& a, const std::string& b) {
  const auto ga = detail::bigrams_of(a);
  const auto gb = detail::bigrams_of(b);
  if (ga.empty() || gb.empty()) return 0;
  const std::size_t inter = detail::multiset_intersection(ga, gb);
  if (inter == 0) return 0;
  const double dice =
      2.0 * static_cast<double>(inter) / static_cast<double>(ga.size() + gb.size());
  int pct = static_cast<int>(std::llround(dice * 100.0));
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  // 二元组完全相同 → 100。Dice 在两侧集合完全相等时必然给出 100，
  // 这里只是把浮点误差兜住。
  if (ga.size() == gb.size() && inter == ga.size() &&
      detail::symbols_of(a) == detail::symbols_of(b)) {
    pct = 100;
  }
  return pct;
}

bool Entry::matches(const std::string& name, int* percent_out) const {
  int best = 0;
  for (const auto& n : detail::names_of(*this)) {
    const int p = percent_match(n, name);
    if (p > best) best = p;
  }
  if (percent_out != nullptr) *percent_out = best;
  return best >= kMatchThreshold;
}

std::string truncate_chars(const std::string& s, std::size_t max_chars) {
  if (max_chars == 0) return {};
  // 按 UTF-8 码点走：每个码点 1~4 字节，逐个推进，
  // 落在字符边界上就不会切出半个字。
  std::size_t chars = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    if ((c & 0xE0) == 0xC0) {
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
    }
    if (chars == max_chars) return s.substr(0, i);
    ++chars;
    i += len;
  }
  return s;
}

bool is_relevant(const std::string& query, const std::string& text,
                 std::string* term_out) {
  if (term_out != nullptr) term_out->clear();
  const std::string trimmed = detail::trim(query);
  if (trimmed.empty() || text.empty()) return false;

  // 梗名是否出现在正文里。
  //
  // 用 percent_match 判「正文里这段是不是在说这个词」是不对的——
  // 这里判的是**抓回来的正文里有没有出现这个梗**，本质是包含判断，
  // 但必须按「字符」而不是按「字节」比，否则中文会切错。
  //
  // 做法：把正文和梗名都切成符号序列，然后在正文里找梗名的子序列。
  const auto needle = detail::symbols_of(trimmed);
  if (needle.empty()) return false;
  const auto hay = detail::symbols_of(text);
  if (hay.size() < needle.size()) return false;

  for (std::size_t start = 0; start + needle.size() <= hay.size(); ++start) {
    bool all = true;
    for (std::size_t k = 0; k < needle.size(); ++k) {
      if (hay[start + k] != needle[k]) {
        all = false;
        break;
      }
    }
    if (all) {
      if (term_out != nullptr) *term_out = trimmed;
      return true;
    }
  }
  return false;
}

// 相关性闸门的**加强版**：光有梗名不够，还得看这段文本是不是在**解释**它。
//
// 为什么需要：只判「梗名出现」的话，「计算器上的 666 是数字游戏里的说法」
// 会被当成「讲清了 666 这个梗」——梗名确实出现了，但它讲的是别的东西。
//
// 加强判据（不判断模型意图，只看文本自身的结构）：
//   正文里出现了「解释性语境词」（意思 / 含义 / 用来 / 表示 / 网络用语 …），
//   或者这段文本本身带了引号 / 书名号（多半是带例句的解释）。
//
// 为什么不做得太严：宁可放过一些噪声，也不能把真梗挡在外面——
// 挡住的后果是模型永远学不到这个梗，代价比库里多一条噪声大得多。
bool is_relevant_with_context(const std::string& query, const std::string& text,
                               std::string* term_out) {
  if (!is_relevant(query, text, term_out)) return false;

  const std::string ntext = normalize_text(text);
  if (ntext.empty()) return false;

  // 语境词表：解释性词汇。只用于校验抓取结果是否在讲这个梗。
  static const char* kContextWords[] = {
      "意思", "含义", "意指", "指的是", "是指", "即为", "意为",
      "用来", "用于", "表示", "释义", "解释",
      "网络用语", "网络流行语", "流行语", "热梗", "黑话", "俚语",
      "俗称", "缩写", "谐音", "口头禅", "用法",
      "形容", "调侃", "自嘲", "出处", "来历", "来源",
  };
  for (const char* w : kContextWords) {
    if (ntext.find(w) != std::string::npos) return true;
  }

  // 正文里带引号片段（多半是带例句的解释）
  if (text.find("「") != std::string::npos ||
      text.find("“") != std::string::npos ||
      text.find("\"") != std::string::npos ||
      text.find("《") != std::string::npos) {
    return true;
  }

  return false;
}

std::vector<std::string> extract_aliases(const std::string& meaning) {
  std::vector<std::string> out;
  if (meaning.empty()) return out;

  // 别名引导词。识别的是**句式**（引导词 + 并列项），不是关键词黑名单。
  static const char* kLeadWords[] = {
      "也叫", "又称", "也称", "又叫", "俗称", "别称", "也俗称", "又俗称",
      "也叫作", "又称作", "也叫成", "又叫作",
  };

  for (const char* lead : kLeadWords) {
    const std::string lead_s = lead;
    std::size_t pos = 0;
    while (true) {
      const auto at = meaning.find(lead_s, pos);
      if (at == std::string::npos) break;
      pos = at + lead_s.size();

      // 引导词之后的那一段，直到**句末**标点为止。
      //
      // 注意：「、」是并列项分隔符，不是句末——「也叫蓝色大肥鱼、吃白饭的大肥鱼」
      // 要当成两个别名，所以不能在这里截断。
      std::size_t end = meaning.size();
      for (std::size_t i = pos; i < meaning.size();) {
        std::size_t len = 1;
        const std::uint32_t cp = detail::decode_utf8(meaning, i, len);
        if (cp == 0x3002 || cp == 0xFF1B || cp == 0xFF01 || cp == 0xFF1F ||
            cp == 0xFF0E || cp == 0x0A || cp == 0x0D) {
          end = i;
          break;
        }
        i += len;
      }
      const std::string seg = detail::trim(meaning.substr(pos, end - pos));
      // 整段太长 = 这不是别名列举，是一整句说明。
      // 先判长度再切分：先切分的话，「……用来描述它的来历**和**背景」
      // 会被切成「背景」这种碎片，反而被当成别名收进去。
      //
      // 长度按**字符数**算，不按字节：中文一个字 3 字节，
      // 按字节判会把「蓝色大肥鱼」（15 字节）误当成超长而丢掉。
      if (!seg.empty() && detail::char_count(seg) <= 24) {
        // 并列项用 、/ ，/ 和/ / 隔开（UTF-8 多字节，不能写成 char 字面量）
        std::vector<std::string> parts;
        std::string cur;
        std::size_t i = 0;
        while (i < seg.size()) {
          std::size_t len = 1;
          const std::uint32_t cp = detail::decode_utf8(seg, i, len);
          if (cp == 0x3001 || cp == 0xFF0C || cp == 0xFF0E || cp == 0x3002 ||
              cp == 0xFF1B || cp == 0xFF0F || cp == '/') {
            if (!cur.empty()) {
              parts.push_back(detail::trim(cur));
              cur.clear();
            }
          } else if (cp == 0x548C) {   // 和
            if (!cur.empty()) {
              parts.push_back(detail::trim(cur));
              cur.clear();
            }
            i += len;
            continue;
          } else {
            cur.append(seg, i, len);
          }
          i += len;
        }
        if (!cur.empty()) parts.push_back(detail::trim(cur));

        for (const auto& p : parts) {
          // 太长的一般不是别名，是一整句话（按字符数算）
          if (detail::char_count(p) < 2 || detail::char_count(p) > 24) continue;
          // 别名不能和已有的重复
          bool dup = false;
          for (const auto& existing : out) {
            if (detail::same_term(existing, p)) dup = true;
          }
          if (!dup) out.push_back(p);
        }
      }
    }
  }
  return out;
}

std::string extract_usage_example(const std::string& text, const std::string& term) {
  if (text.empty() || term.empty()) return {};
  const auto nterm = detail::symbols_of(term);
  if (nterm.empty()) return {};

  // 常见的成对引号：中文直角引号 / 书名号 / 弯引号 / 直引号
  static const char* kPairs[][2] = {
      {"「", "」"}, {"『", "』"}, {"《", "》"}, {"【", "】"},
      {"“", "”"}, {"‘", "’"}, {"\"", "\""},
  };
  for (const auto& p : kPairs) {
    const std::string open(p[0]);
    const std::string close(p[1]);
    std::size_t pos = 0;
    while (true) {
      const auto a = text.find(open, pos);
      if (a == std::string::npos) break;
      const auto b = text.find(close, a + open.size());
      if (b == std::string::npos) break;
      const std::string inner = text.substr(a, b + close.size() - a);
      if (is_relevant(term, inner, nullptr)) return inner;
      pos = b + close.size();
    }
  }
  return {};
}

// ── MemeStore ───────────────────────────────────────────

MemeStore::MemeStore(Config cfg) : cfg_(std::move(cfg)) {}

std::string MemeStore::file_path() const { return cfg_.dir + "/memes.tsv"; }

std::string MemeStore::tmp_path() const {
  // 每次写用不同的临时文件名：多个会话同时学同一个梗时，
  // 共用一个 .tmp 会互相覆盖，rename 出来的文件就是坏的。
  static std::atomic<unsigned long long> seq{0};
  const auto n = seq.fetch_add(1) + 1;
  return file_path() + ".tmp." + std::to_string(n);
}

fish::core::Error MemeStore::init() {
  std::error_code ec;
  fs::create_directories(cfg_.dir, ec);
  if (ec) {
    return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                   "mkdir_memes:" + ec.message());
  }
  return load();
}

fish::core::Error MemeStore::reload() { return load(); }

fish::core::Error MemeStore::load() {
  std::vector<Entry> loaded;
  std::ifstream in(file_path());
  if (in) {
    std::string line;
    while (std::getline(in, line)) {
      line = detail::trim(line);
      if (line.empty() || line[0] == '#') continue;
      // 跳过表头：表头可能不在第一行（前面常有注释），所以每行都判。
      if (line.rfind("term\t", 0) == 0) continue;
      // 格式：term \t meaning \t usage \t origin \t added_at \t hits
      //       \t aliases \t edited
      // 后两列是可选的：老文件只有 6 列，照样能读。
      const auto f = detail::split(line, '\t');
      if (f.size() < 2) continue;
      Entry e;
      e.term = detail::trim(f[0]);
      e.meaning = detail::trim(f[1]);
      if (f.size() > 2) e.usage = detail::trim(f[2]);
      if (f.size() > 3) e.origin = detail::trim(f[3]);
      if (f.size() > 4) e.added_at = detail::trim(f[4]);
      if (f.size() > 5) {
        try {
          e.hits = std::stoi(detail::trim(f[5]));
        } catch (...) {
        }
      }
      if (f.size() > 6) {
        // 别名用 | 分隔（别名本身可能含逗号，用 | 更安全）
        for (const auto& a : detail::split(f[6], '|')) {
          const std::string t = detail::trim(a);
          if (!t.empty()) e.aliases.push_back(t);
        }
      }
      if (f.size() > 7) {
        e.edited = detail::trim(f[7]) == "1" || detail::trim(f[7]) == "true";
      }
      if (e.term.empty()) continue;

      // 别名兜底：从释义里自动抽（「也叫X、Y」这种句式）。
      // TSV 里已经写了 aliases 的条目以文件为准，不重复抽。
      if (e.aliases.empty()) {
        for (const auto& a : extract_aliases(e.meaning)) {
          // 别名不能等于梗名本身
          if (detail::same_term(a, e.term)) continue;
          e.aliases.push_back(a);
        }
      }
      loaded.push_back(std::move(e));
    }
  }

  std::lock_guard<std::mutex> lock(mu_);
  items_ = std::move(loaded);
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "meme", "loaded")
      .field("count", static_cast<long long>(items_.size()))
      .field("dir", cfg_.dir);
  return fish::core::Error::none();
}

fish::core::Error MemeStore::save_locked() const {
  const std::string path = file_path();
  const std::string tmp = tmp_path();
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      return fish::core::Error::make(fish::core::ErrorCode::kInternal, "memes_open");
    }
    out << "# 网络热梗库\n";
    out << "# 格式: term<TAB>meaning<TAB>usage<TAB>origin<TAB>added_at<TAB>hits"
           "<TAB>aliases<TAB>edited\n";
    out << "# aliases 是别名/变体，用 | 分隔（可留空）。\n";
    out << "# edited=1 表示释义被纠正过，自动学习不会再覆盖它。\n";
    out << "# 直接手改这个文件即可增删，重启或 reload() 后生效。\n";
    out << "term\tmeaning\tusage\torigin\tadded_at\thits\taliases\tedited\n";
    for (const auto& e : items_) {
      out << e.term << '\t' << e.meaning << '\t' << e.usage << '\t' << e.origin << '\t'
          << e.added_at << '\t' << e.hits << '\t';
      for (std::size_t i = 0; i < e.aliases.size(); ++i) {
        if (i > 0) out << '|';
        out << e.aliases[i];
      }
      out << '\t' << (e.edited ? "1" : "0") << '\n';
    }
    out.flush();
    if (!out) {
      return fish::core::Error::make(fish::core::ErrorCode::kInternal, "memes_write");
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    // 目标已存在且平台不允许覆盖时：删旧再改名。
    // 只在「目标确实存在」时才删——否则会把「改名失败」变成「数据丢失」。
    std::error_code exists_ec;
    if (fs::exists(path, exists_ec)) {
      std::error_code rm_ec;
      fs::remove(path, rm_ec);
      if (!rm_ec) {
        ec.clear();
        fs::rename(tmp, path, ec);
      }
    }
  }
  if (ec) {
    std::error_code rm_ec;
    fs::remove(tmp, rm_ec);
    return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                   "memes_rename:" + ec.message());
  }
  return fish::core::Error::none();
}

std::size_t MemeStore::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return items_.size();
}

std::vector<Entry> MemeStore::all() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Entry> out = items_;
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    if (a.added_at != b.added_at) return a.added_at > b.added_at;
    return a.term < b.term;
  });
  return out;
}

bool MemeStore::find_locked(const std::string& name, Match* out) const {
  if (name.empty()) return false;
  int best = 0;
  for (const auto& e : items_) {
    int pct = 0;
    if (!e.matches(name, &pct)) continue;
    if (pct > best) {
      best = pct;
      if (out != nullptr) {
        out->entry = e;
        out->percent = pct;
        out->exact = pct >= 100;
      }
      // 100 分不用说，已经是最高分，直接收工
      if (pct >= 100) return true;
    }
  }
  return best >= kMatchThreshold;
}

bool MemeStore::find(const std::string& name, Match* out) const {
  std::lock_guard<std::mutex> lock(mu_);
  return find_locked(name, out);
}

fish::core::Error MemeStore::correct(const std::string& term, const std::string& meaning,
                                     Match* out) {
  if (term.empty()) {
    return fish::core::Error::make(fish::core::ErrorCode::kBadRequest, "meme_empty_term");
  }
  if (meaning.empty()) {
    return fish::core::Error::make(fish::core::ErrorCode::kBadRequest, "meme_empty_meaning");
  }
  std::lock_guard<std::mutex> lock(mu_);

  Match m;
  if (!find_locked(term, &m)) {
    return fish::core::Error::make(fish::core::ErrorCode::kNotFound, "meme_not_found");
  }

  // 按匹配到的名字反查到底是哪一条：find_locked 返回的是副本，
  // 这里要在容器里找到同一个 term 的那条才能改。
  auto it = std::find_if(items_.begin(), items_.end(), [&](const Entry& e) {
    return e.term == m.entry.term;
  });
  if (it == items_.end()) {
    return fish::core::Error::make(fish::core::ErrorCode::kNotFound, "meme_not_found");
  }

  it->meaning = meaning;
  it->edited = true;
  // 纠正后的释义里可能带「也叫 X」句式，顺手把别名补齐，
  // 这样纠正完的名字也参与匹配。
  for (const auto& a : extract_aliases(meaning)) {
    if (detail::same_term(a, it->term)) continue;
    bool dup = false;
    for (const auto& existing : it->aliases) {
      if (detail::same_term(existing, a)) dup = true;
    }
    if (!dup) it->aliases.push_back(a);
  }

  const auto err = save_locked();
  if (!err.ok()) return err;

  if (out != nullptr) {
    out->entry = *it;
    out->percent = m.percent;
    out->exact = m.exact;
  }
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "meme", "corrected")
      .field("term", it->term)
      .field("percent", static_cast<long long>(m.percent));
  return fish::core::Error::none();
}

bool MemeStore::has(const std::string& term) const {
  if (term.empty()) return false;
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& e : items_) {
    if (detail::same_term(e.term, term)) return true;
    for (const auto& a : e.aliases) {
      if (detail::same_term(a, term)) return true;
    }
  }
  return false;
}

fish::core::Error MemeStore::upsert(const Entry& e) {
  if (e.term.empty()) {
    return fish::core::Error::make(fish::core::ErrorCode::kBadRequest, "meme_empty_term");
  }
  std::lock_guard<std::mutex> lock(mu_);
  bool found = false;
  for (auto& existing : items_) {
    // 同名或同别名都算同一条
    bool same = detail::same_term(existing.term, e.term);
    if (!same) {
      for (const auto& a : existing.aliases) {
        if (detail::same_term(a, e.term)) {
          same = true;
          break;
        }
      }
    }
    if (!same) {
      for (const auto& a : e.aliases) {
        if (detail::same_term(a, existing.term)) {
          same = true;
          break;
        }
      }
    }
    if (!same) continue;

    // 保留原 hits，更新内容
    const int old_hits = existing.hits;
    existing = e;
    existing.hits = old_hits;
    if (existing.added_at.empty()) existing.added_at = detail::today_str();
    found = true;
    break;
  }
  if (!found) {
    Entry copy = e;
    if (copy.added_at.empty()) copy.added_at = detail::today_str();
    items_.push_back(std::move(copy));
  }
  return save_locked();
}

int MemeStore::import_many(const std::vector<Entry>& entries) {
  int added = 0;
  int filled = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& e : entries) {
      if (e.term.empty()) continue;

      // 查重：库里已有同名 / 同别名 → 视为同一条。
      //
      // 比对的是**所有名字**（term + 别名）对**所有名字**：
      //   existing.term  vs e.term
      //   existing.term  vs e.aliases[*]
      //   existing.alias vs e.term
      //   existing.alias vs e.aliases[*]
      // 少比一种就会漏：「别名撞别名」是最容易漏的一对。
      Entry* hit = nullptr;
      auto names_match = [](const Entry& x, const std::vector<std::string>& enames) {
        for (const auto& xn : detail::names_of(x)) {
          for (const auto& y : enames) {
            if (detail::same_term(xn, y)) return true;
          }
        }
        return false;
      };
      const auto enames = detail::names_of(e);
      for (auto& existing : items_) {
        if (names_match(existing, enames)) {
          hit = &existing;
          break;
        }
      }

      if (hit != nullptr) {
        // 已经存在：**绝不覆盖纠正过的释义**。
        //
        // 自动学来的东西质量参差，人工 / 模型纠正过的才是可信的；
        // 覆盖回去等于把纠正扔了，下次又错。
        // 只补空缺（usage / origin），并且只在原值为空时补。
        bool touched = false;
        if (!hit->edited && !e.meaning.empty()) {
          // 老条目释义为空或明显更短时，用新学到的补上
          if (hit->meaning.empty()) {
            hit->meaning = e.meaning;
            touched = true;
          }
        }
        if (hit->usage.empty() && !e.usage.empty()) {
          hit->usage = e.usage;
          touched = true;
        }
        if (hit->origin.empty() && !e.origin.empty()) {
          hit->origin = e.origin;
          touched = true;
        }
        if (touched) ++filled;
        continue;
      }

      Entry copy = e;
      if (copy.added_at.empty()) copy.added_at = detail::today_str();
      items_.push_back(std::move(copy));
      ++added;
    }
    if (added > 0 || filled > 0) {
      const auto err = save_locked();
      if (!err.ok()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "meme", "save_failed")
            .field("detail", err.detail);
      }
    }
  }
  if (added > 0 || filled > 0) {
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "meme", "imported")
        .field("added", static_cast<long long>(added))
        .field("filled", static_cast<long long>(filled));
  }
  return added;
}

}  // namespace fish::meme
