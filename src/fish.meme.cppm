// src/fish.meme.cppm
// 网络热梗库：把清单交给模型自己挑，外部学到的写回本地。
//
// 设计：
//   - 独立存储区（data/memes/），与记忆分开：
//     热梗是「公共知识」，不随会话隔离；记忆是「私人上下文」
//   - 权威源是纯文本文件（人可读、可手改、可增删）
//   - **不做检索**：all() 列出全部条目，模型自己看清单挑；
//     detail(index) 按序号取一条。程序不参与「这条是不是用户问的那个」的判断
//     ——那是模型的事，程序只负责如实摆出有哪些条目。
//   - 清单里没有的梗，由模型点名（learn 参数）后联网学一次并写回
//
// 为什么需要：
//   模型训练数据有截止日期，新梗它不知道。若每次都联网查，既慢又贵。
//   所以本地建库：学到一次就记住，之后零成本。
//
// 为什么不做子串匹配（历史教训）：
//   以前 search(query) 用双向子串去匹配 term/释义/用法，试图「猜」模型在问哪个梗。
//   结果是「这个」「意思」这类片段命中大半张表，模型拿到一堆不相干的条目，
//   反而更看不懂；真正想看的条目还常被 max_results 截掉。
//   现在改成列清单：程序不做任何判断，模型读到清单自己选，准确率反而更高。
module;

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
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
  // 现在不再用于「检索匹配」（已经没有检索了），只用于两件事：
  //   1. 写回查重：别名撞别名也算同一个梗，不重复入库
  //   2. 清单里给模型看：「梗名（也叫 别名1、别名2）」
  //      ——用户可能用的是别名或者另一种写法，模型看到就知道是同一条。
  std::vector<std::string> aliases;
  // 被取详情次数（index 取一条时 +1）。
  //
  // 语义已变：以前是「search 命中次数」（翻清单也算命中，没意义），
  // 现在是「这条真被模型拿去用了几次」。TSV 结构不变，存量数据照样读。
  int hits{0};
};

// 规范化：全角→半角、ASCII 小写、去掉所有空白。
//
// 现在的用途只剩「判断两个写法是不是同一个词」：
//   - 写回查重（same_term）
//   - 别名 / 变体等价（拴Q ↔ 栓Q）
// 它**不参与**「猜用户问的是哪个梗」——那件事已经没有了。
//
// 非 ASCII（中文等）原样保留：中文没有大小写 / 全角问题，
// 写法变体（拴Q / 栓Q）由变体表处理。
std::string normalize_text(const std::string& s);

// 相关性闸门：抓回来的正文是否**确实在讲这个梗**。
//
// 【与「关键词匹配」的区别，务必分清】
//   这里判的是「**联网抓回来的第三方内容**跟我们要学的那个梗有没有关系」，
//   是数据入库前的质量闸门：搜「班味」搜到班组长的值班表，这种内容
//   写进库会污染本地数据，越查越烂。
//   它**不是**「判断用户这句话在问什么」，也不参与模型侧的选梗——
//   模型问哪个梗由模型自己从清单里挑，程序不猜。
//   用户反对的是后者（程序替模型猜意图），不是前者（脏数据入库）。
//
// term_out 出参：正文里实际出现的那个梗名（写回时用它当条目的 term）。
// 相关时返回 true。
bool is_relevant(const std::string& query, const std::string& text,
                 std::string* term_out = nullptr);

// 从释义里自动抽取别名 / 变体。
//
// 为什么需要：存量热梗的别名是**混在释义的自然语言里**的，
// 抽出来当别名，清单里能一并展示，模型也就知道「用户说的那个写法」是哪条。
// 这是**数据增强**，不是匹配。
//
// 识别的是**结构化的引导词**（也叫 / 又叫 / 也称 / 又称 / 俗称 / 别称），
// 不是关键词黑名单：它不判断「这条释义在讲什么」，
// 只看有没有「别名引导词 + 一串并列项」这个句式。
//
// 返回抽到的别名（可能为空）。
std::vector<std::string> extract_aliases(const std::string& meaning);

// 相关性闸门（加强版）：梗名出现在文本里 **且** 这段文本像是在解释它。
//
// 判据说明：
//   - 「梗名是否出现在抓回来的文本里」是核心判据
//   - 额外要求文本里有解释性语境，否则「梗名出现了但讲的是别的东西」
//     也会被当成查到了
//
// 同上：这是**抓取结果入库前的质量闸门**，不是判断用户意图。
// 详细理由见实现处注释。
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
  int import_many(const std::vector<Entry>& entries);

  [[nodiscard]] std::size_t size() const;

  // 全部条目（快照）。**清单工具就用这个**——不检索、不截断。
  //
  // 排序：added_at 倒序（新的在前），同一天按 term 字典序兜底。
  //   - 新的在前：新梗更可能被问
  //   - 同一天按 term：序号必须稳定，模型拿清单后要带 index 回来取详情，
  //     两次调用之间顺序一变就会取错条目
  //
  // 这里**不设条数上限**：模型上下文够大，截断会让模型以为「库里就这么多」，
  // 反而找不到用户说的那条。库真的长到几百条该处理的是数据规模，不是悄悄截断。
  std::vector<Entry> all() const;

  // 按「清单里的序号」取一条（0 基，序号即 all() 的下标）。
  // 命中时给这条 hits +1 并落盘（这才是「真的被用到」）。
  //
  // 返回 false = 序号越界或库为空，调用方据此告诉模型「序号不对」。
  bool detail(std::size_t index, Entry* out);

 private:
  fish::core::Error load();
  // 落盘。调用方必须持有 mu_。
  fish::core::Error save_locked() const;
  [[nodiscard]] std::string file_path() const;
  // 写临时文件用的唯一名：多个会话同时学同一个梗时，
  // 共用一个 .tmp 会互相覆盖，rename 出来的文件就是坏的。
  [[nodiscard]] std::string tmp_path() const;

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
  if (cp >= 0xFF01 && cp <= 0xFF20) return true;   // 全角标点
  if (cp >= 0xFF3B && cp <= 0xFF40) return true;   // 全角标点
  if (cp >= 0xFF5B && cp <= 0xFF65) return true;   // 全角标点
  return false;
}

// 常见变体：写法不同但说的是同一个梗。
//
// 只收「实际会被混用」的字，不做通用模糊匹配——
// 模糊匹配会把不相关的词也拉进来，模型会以为自己知道了。
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

// hay 里是否含 needle。
//
// 现存调用点只有 text_has_form()（相关性闸门的一部分：判断第三方抓回来的正文
// 里有没有出现这个梗），已经不再有任何「拿用户查询串去匹配库条目」的用法。
bool contains(const std::string& hay, const std::string& needle) {
  if (needle.empty() || hay.size() < needle.size()) return false;
  return hay.find(needle) != std::string::npos;
}

// text 是否包含 form（含变体等价的写法）。
//
// form 可能是**没规范化**的原文（"栓Q"），text 也可能是原文。
// 两侧都要规范化后再比，否则大小写 / 全角差异会漏。
bool text_has_form(const std::string& text, const std::string& form) {
  if (form.empty()) return false;
  const std::string nt = normalize_text(text);
  const std::string nf = normalize_text(form);
  if (nf.empty()) return false;
  if (contains(nt, nf)) return true;
  // 变体等价：拴Q ↔ 栓Q
  const std::string nt_var = apply_variants(nt);
  const std::string nf_var = apply_variants(nf);
  if (nf_var != nf && contains(nt_var, nf_var)) return true;
  return contains(nt_var, nf);
}

// 查询串按空白 / 标点切成片段（保留原文形态）。
//
// 现在只服务于 is_relevant()：模型/调用方给的梗名偶尔带问法
// （「X 是什么意思」），切段后按段判相关性。
std::vector<std::string> split_segments(const std::string& query) {
  std::vector<std::string> out;
  std::string cur;
  std::size_t i = 0;
  while (i < query.size()) {
    std::size_t len = 1;
    const std::uint32_t cp = decode_utf8(query, i, len);
    if (is_separator_cp(cp)) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.append(query, i, len);
    }
    i += len;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
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

bool is_relevant(const std::string& query, const std::string& text,
                 std::string* term_out) {
  if (term_out != nullptr) term_out->clear();
  const std::string trimmed = detail::trim(query);
  if (trimmed.empty() || text.empty()) return false;

  // 1. 梗名（整串）出现在正文里 → 相关。
  //
  // 不再从查询串里「猜」梗名（原 term_candidate 的启发式已删）：
  // 联网学习现在只由 learn 参数触发，传进来的就是模型明确点名的新梗，
  // 不再有「这个 X 是什么意思」这种整句被人硬塞进来的情况。
  if (detail::text_has_form(text, trimmed)) {
    if (term_out != nullptr) *term_out = trimmed;
    return true;
  }

  // 2. 片段级：查询串里某个片段出现在正文里（长度≥2，避免单字误命中）。
  //    留作兜底：调用方（测试 / 手工）仍可能传带空格的写法。
  const auto segs = detail::split_segments(trimmed);
  std::string best;
  for (const auto& seg : segs) {
    if (seg.size() < 2) continue;
    if (!detail::text_has_form(text, seg)) continue;
    if (seg.size() > best.size()) best = seg;
  }
  if (!best.empty()) {
    if (term_out != nullptr) *term_out = best;
    return true;
  }

  return false;
}

// 相关性闸门的**加强版**：光有梗名不够，还得看这段文本是不是在**解释**它。
//
// 为什么需要：只判「梗名出现」的话，「计算器上的 666 是数字游戏里的说法」
// 会被当成「讲清了 666 这个梗」——梗名确实出现了，但它讲的是别的东西。
//
// 加强判据（不判断模型意图，只看文本自身的结构）：
//   梗名附近（前后一定距离内）出现了「解释性语境词」
//   （意思 / 含义 / 意思�� / 指 / 用来 / 表示 / 释义 / 网络用语 / 梗 / 俗称 …），
//   或者这段文本本身就提到了「梗 / 网络用语 / 缩写 / 黑话」这类词。
//
// 两条都判不出来 → 认为「没查到可靠解释」，不写回本地库。
//
// 为什么不做得太严：宁可放过一些噪声，也不能把真梗挡在外面——
// 挡住的后果是模型永远学不到这个梗，代价比库里多一条噪声大得多。
bool is_relevant_with_context(const std::string& query, const std::string& text,
                               std::string* term_out) {
  if (!is_relevant(query, text, term_out)) return false;

  const std::string ntext = normalize_text(text);
  if (ntext.empty()) return false;

  // 语境词表：解释性词汇。
  //
  // 只在这里用（校验抓取结果是否在讲这个梗），不用于判断模型在问什么。
  // 刻意**不含**单独的「梗」字：正文里的「跟网络梗无关」会把它带出来，
  // 那样「计算器上的 666 是数字游戏里的说法」也会被当成查到了。
  static const char* kContextWords[] = {
      "意思", "含义", "意指", "指的是", "是指", "即为", "意为",
      "用来", "用于", "表示", "释义", "解释", "指的是",
      "网络用语", "网络流行语", "流行语", "热梗", "黑话", "俚语",
      "俗称", "缩写", "谐音", "口头禅", "用法", "什么意思", "啥意思",
      "形容", "调侃", "自嘲", "出处", "来历", "来源", "就是",
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
      // 真正的句末标点：。；！？ 以及换行。
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
  const std::string nterm = normalize_text(term);
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
      if (detail::text_has_form(inner, nterm)) return inner;
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
      // 格式：term \t meaning \t usage \t origin \t added_at \t hits \t aliases
      // aliases 是新增的第 7 列（可选）：老文件只有 6 列，照样能读。
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
      if (e.term.empty()) continue;

      // 别名兜底：从释义里自动抽（「也叫X、Y」这种句式）。
      //
      // 存量 24 条热梗的别名都混在释义的自然语言里，
      // 抽出来当别名，模型查「蓝色大肥鱼」也能命中「大肥鱼」那条。
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
           "<TAB>aliases\n";
    out << "# aliases 是别名/变体，用 | 分隔（可留空）。\n";
    out << "# 直接手改这个文件即可增删，重启或 reload() 后生效。\n";
    out << "term\tmeaning\tusage\torigin\tadded_at\thits\taliases\n";
    for (const auto& e : items_) {
      out << e.term << '\t' << e.meaning << '\t' << e.usage << '\t' << e.origin << '\t'
          << e.added_at << '\t' << e.hits << '\t';
      for (std::size_t i = 0; i < e.aliases.size(); ++i) {
        if (i > 0) out << '|';
        out << e.aliases[i];
      }
      out << '\n';
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
  // 清单顺序 = 序号顺序，详见头文件里的排序说明。
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    if (a.added_at != b.added_at) return a.added_at > b.added_at;
    return a.term < b.term;
  });
  return out;
}

bool MemeStore::detail(std::size_t index, Entry* out) {
  std::lock_guard<std::mutex> lock(mu_);
  // 按下标取：下标来自 all() 同一个排序，所以两边必须用同一套比较。
  std::vector<std::size_t> order(items_.size());
  for (std::size_t i = 0; i < items_.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
    const Entry& x = items_[a];
    const Entry& y = items_[b];
    if (x.added_at != y.added_at) return x.added_at > y.added_at;
    return x.term < y.term;
  });
  if (index >= order.size()) return false;

  auto& pick = items_[order[index]];
  // 取详情才算「被用到」：hits +1 并落盘（翻清单不计数）。
  ++pick.hits;
  if (out != nullptr) *out = pick;
  const auto err = save_locked();
  if (!err.ok()) {
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "meme", "save_failed")
        .field("detail", err.detail);
  }
  return true;
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
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& e : entries) {
      if (e.term.empty()) continue;

      // 查重：库里已有同名 / 同别名 → 跳过。
      //
      // 比对的是**所有名字**（term + 别名）对**所有名字**：
      //   existing.term  vs e.term
      //   existing.term  vs e.aliases[*]
      //   existing.alias vs e.term
      //   existing.alias vs e.aliases[*]
      // 少比一种就会漏：「别名撞别名」是最容易漏的一对。
      bool dup = false;
      auto names_of = [](const Entry& x) {
        std::vector<std::string> v{x.term};
        for (const auto& a : x.aliases) v.push_back(a);
        return v;
      };
      const auto enames = names_of(e);
      for (const auto& existing : items_) {
        const auto xnames = names_of(existing);
        for (const auto& x : xnames) {
          for (const auto& y : enames) {
            if (detail::same_term(x, y)) {
              dup = true;
              break;
            }
          }
          if (dup) break;
        }
        if (dup) break;
      }
      if (dup) continue;

      Entry copy = e;
      if (copy.added_at.empty()) copy.added_at = detail::today_str();
      items_.push_back(std::move(copy));
      ++added;
    }
    if (added > 0) {
      const auto err = save_locked();
      if (!err.ok()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "meme", "save_failed")
            .field("detail", err.detail);
      }
    }
  }
  if (added > 0) {
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "meme", "imported")
        .field("added", static_cast<long long>(added));
  }
  return added;
}

}  // namespace fish::meme
