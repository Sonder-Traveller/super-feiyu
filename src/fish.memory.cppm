// src/fish.memory.cppm
// 三层记忆：L2 短时（内存）+ L3 长时（旁路 Markdown 权威 + SQLite 可重建索引）
module;
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

export module fish.memory;

import fish.core;
import fish.log;

export namespace fish::memory {

struct Message {
  std::string role;
  std::string text;
  fish::core::TimePoint at{};
  // 该消息附带的图片本地路径（媒体缓存里的副本）。
  // 空表示纯文本。
  //
  // 为什么必须存：平台给的是临时签名 URL，过期成死链；
  // 若只存 "[发了一张照片]" 这样的文字标记，下一轮模型就真的
  // 看不到那张图了，会说出「我看不到自己照片长什么样」。
  std::vector<std::string> image_paths;
  // 发言人信息（user 侧才有意义）。
  // 群聊里必须保留：否则模型分不清是群里哪个人说的。
  fish::core::Speaker speaker;
  // 发送时刻（unix 毫秒），让模型理解"昨天""刚才"这类时间指代
  std::int64_t at_ms{0};
  // 服务端联网工具（deepseek_web_search / deepseek_read_url）的原始调用
  // 记录与引用，JSON 文本。
  //
  // 为什么单独存字段而不是塞进 text：
  //   塞进 text 的做法（\x01stc:...\x01ann:...\x01）会让这串控制字符
  //   一起进 episodes.md、进 SQLite 向量索引、还被喂给 consolidate 的
  //   LLM —— 模型看到的是乱码，还白烧 token。
  // 单独存字段后：记忆里的正文始终干净，回传时从这两个字段取。
  std::string server_tool_calls_json;
  std::string annotations_json;
};

struct Fact {
  std::string id;
  std::string subject{"用户"};
  std::string content;
  double confidence{0.9};
  std::string created;
  std::string superseded_by;
  std::string batch_id;   // 产生它的巩固批次，用于幂等去重
};

struct Episode {
  std::string id;
  std::string summary;
  double importance{0.5};
  std::string created;
};

struct Trait {
  std::string pattern;
  int evidence{1};
};

struct MemoryContext {
  std::vector<Message> working;   // 预算内全量的最近原文
  std::string summary;            // 更早内容的滚动摘要
  std::size_t estimated_tokens{0};
};

// 估算 token：中文按 1 字≈1，英文按 4 字符≈1。粗略但够分配预算用。
std::size_t estimate_tokens(const std::string& text);

// 上下文预算：按模型窗口的百分比切分，取代固定轮数
struct TokenBudget {
  // 原文缓冲（buffer）的硬上限：最多占模型窗口的 50%。
  //
  // 这是用户明确批准的一条比例上限（"给 buffer 加上限，50% 吧"）。
  // 它是天花板，不是默认值：配置里写多高都会被压回这个数，
  // 所以任何一条会话的原文缓冲都不会超过窗口的一半。
  static constexpr double kRawBufferMaxRatio = 0.50;
  // 配置缺省 / 非法时的取值。故意低于上限：没配置就按保守的来，
  // 不因为多了一条上限就顺手把实际用量抬上去。
  static constexpr double kRawBufferDefaultRatio = 0.40;
  // 原文缓冲比例的**下限**。
  //
  // 为什么需要下限：raw_context_ratio 配得过小（例如 0.01）时，
  // 每轮对话都会立刻超预算 → 每轮都触发 compact() → 每轮都调用一次
  // 巩固 LLM（一次摘要 + 一次事实提炼）→ 又是钱又是延迟，
  // 而且压缩出来的摘要质量很差（压缩比太狠）。
  // 低于下限时按下限处理并打 warn 日志，不静默照做。
  static constexpr double kRawBufferMinRatio = 0.05;
  // 建议的模型窗口上限：超过这个值时启动会提醒用户核对配置。
  //
  // 为什么需要：model_context_tokens 配得比真实模型窗口大（例如配 1M、
  // 实际模型只有 128K）时，代码算出来的预算永远用不满，
  // 压缩永远不会触发，请求却会被上游直接拒掉。
  // 这里只发告警，不自动改配置 —— 改配置是运维的决定。
  static constexpr int kLikelyOversizedContextTokens = 262144;

  std::size_t total{0};
  std::size_t raw_budget{0};      // 原文可用（<= total * kRawBufferMaxRatio）
  std::size_t summary_budget{0};  // 摘要可用
  std::size_t reserve{0};         // 留给回复与工具定义

  static TokenBudget make(int model_context_tokens, double raw_ratio, double summary_ratio);
};


struct ConsolidateReport {
  int episodes_written{0};
  int facts_written{0};
  int traits_updated{0};
  bool skipped{false};
  std::string reason;
};

// ── L2 短时记忆 ─────────────────────────────────────────
class WorkingMemory {
 public:
  // 0 = 不按轮数截断（默认）。截断只能靠显式传参，不能靠默认值偷偷加上限。
  explicit WorkingMemory(int max_rounds = 0);
  void push(const Message& user, const Message& assistant);
  [[nodiscard]] std::vector<Message> snapshot() const;
  [[nodiscard]] int round_count() const;
  std::vector<Message> take_all();
  void clear();
  void restore(const std::vector<Message>& msgs);

 private:
  int max_rounds_;
  mutable std::mutex mu_;
  std::deque<Message> msgs_;
};

// ── 旁路文件（权威源）────────────────────────────────────
class NotesStore {
 public:
  explicit NotesStore(std::string notes_dir);
  fish::core::Error ensure_dirs() const;

  // 会话隔离：每个会话一个子目录，互不可见。
  // 此前所有会话共享同一个 facts.md / episodes/ / traits.md，
  // 导致 A 会话的事实和情景记忆会串到 B 会话里。
  [[nodiscard]] std::string session_dir(const std::string& conv_id) const;

  // 旧编码的目录名（':' 一律换成 '_'）。
  //
  // 用途：兼容**已存在的旧数据目录**。新编码把 ':' 写成 "__COLON__"，
  // 旧目录名是 "g_<openid>" 形式，读取时若新目录不存在就回退到旧目录，
  // 这样已有的事实/情景记忆不会因为改编码而失效。
  [[nodiscard]] std::string session_dir_legacy(const std::string& conv_id) const;

  // **读**用哪个目录：新目录优先，没有就回退旧目录。
  //
  // 写永远写新目录（session_dir）。启动时的 migrate_legacy_dirs() 会把
  // 旧目录改名过来；迁移失败（权限等）时至少还能读到旧数据。
  [[nodiscard]] std::string session_dir_read(const std::string& conv_id) const;

  // 从磁盘目录名反推 conv_id。
  //
  // 新编码：目录名里的 "__COLON__" 还原成 ':'；kind=="group" 时补回
  // "g:" 前缀（目录名里存的是去掉前缀后的 openid）。
  // 旧编码：目录名里没有 ':' 痕迹，只能按 kind 补 "g:" 前缀 ——
  // 这正是原来反推多一个 "g:" 的地方（"g:" + "g_<openid>"）。
  [[nodiscard]] static std::string conv_id_from_dir(const std::string& kind,
                                                    const std::string& dir_name);

  fish::core::Result<std::vector<Fact>> load_facts(const std::string& conv_id) const;
  fish::core::Error append_fact(const std::string& conv_id, const Fact& f) const;
  // 全量改写（按 subject 分组），用于合并后重写权威文件
  fish::core::Error rewrite_facts(const std::string& conv_id,
                                  const std::vector<Fact>& facts) const;
  fish::core::Error append_episode(const std::string& conv_id, const Episode& e) const;
  // 全量改写情景记忆（按月份分文件），用于压缩后重写
  fish::core::Error rewrite_episodes(const std::string& conv_id,
                                     const std::vector<Episode>& eps) const;
  fish::core::Result<std::vector<Episode>> load_episodes(const std::string& conv_id,
                                                         int max_months = 24) const;
  fish::core::Result<std::vector<Trait>> load_traits(const std::string& conv_id) const;
  fish::core::Error save_traits(const std::string& conv_id,
                                const std::vector<Trait>& traits) const;

 private:
  fish::core::Error ensure_session_dir(const std::string& conv_id) const;

  // 旧编码目录 → 新编码目录的一次性迁移（启动时做，见实现）。
  void migrate_legacy_dirs() const;

  std::string dir_;
  mutable std::mutex mu_;
};

// ── SQLite 索引（可重建）──────────────────────────────────
class IndexStore {
 public:
  explicit IndexStore(std::string db_path);
  ~IndexStore();
  IndexStore(const IndexStore&) = delete;
  IndexStore& operator=(const IndexStore&) = delete;

  fish::core::Error open();
  fish::core::Error close();

  // 索引一条事实/情景记忆。
  //
  // conv_id 是**必填**：索引按会话隔离。没有 conv_id 的条目无法归属，
  // 检索时永远不会被返回（见缺陷 2：跨会话泄漏是隐私问题）。
  fish::core::Error upsert_item(const std::string& conv_id, const std::string& kind,
                                const std::string& id, const std::string& text,
                                const std::vector<float>& vec);
  fish::core::Error remove_item(const std::string& conv_id, const std::string& kind,
                                const std::string& id);

  struct Hit {
    std::string kind;
    std::string id;
    std::string text;
    double score{0.0};
  };

  // 向量召回。conv_id 为空串表示「不按会话过滤」——
  // 只用于内部诊断/重建，**不要**拿它做对外检索。
  fish::core::Result<std::vector<Hit>> search(const std::vector<float>& query, int top_k,
                                               const std::string& conv_id,
                                               const std::string& kind = "") const;
  // 关键词召回（LIKE + C++ 侧相关度排序，见缺陷 5/6）。
  fish::core::Result<std::vector<Hit>> search_text(const std::string& conv_id,
                                                   const std::string& keyword,
                                                   int top_k) const;
  [[nodiscard]] std::size_t item_count() const;
  // 已归属到某个会话的条目数（conv_id 非空）。用于和真源文件比对。
  [[nodiscard]] std::size_t indexed_item_count() const;
  // 清空索引（重建前的第一步）。只清索引，不碰旁路 Markdown。
  fish::core::Error clear();

 private:
  std::string path_;
  void* db_{nullptr};
  // 索引可能被并发访问：搜索线程 + 后台重建线程。
  mutable std::mutex mu_;
};

// ── 巩固器 ──────────────────────────────────────────────
class Consolidator {
 public:
  Consolidator(fish::core::IChatModel* chat, NotesStore* notes, IndexStore* index,
               fish::core::IEmbeddingModel* embedding);
  fish::core::Result<ConsolidateReport> consolidate(const std::string& conv_id,
                                                    const std::vector<Message>& transcript);
  // 把一段较早的对话压成摘要文本
  fish::core::Result<std::string> summarize(const std::vector<Message>& transcript);
  static fish::core::Result<ConsolidateReport> apply_json(
      const std::string& json_text, const std::string& today, NotesStore* notes,
      IndexStore* index, fish::core::IEmbeddingModel* embedding,
      const std::string& conv_id);

 private:
  fish::core::IChatModel* chat_;
  NotesStore* notes_;
  IndexStore* index_;
  fish::core::IEmbeddingModel* embedding_;
};

// ── 统一门面 ────────────────────────────────────────────
class MemorySystem {
 public:
  struct Config {

    // ── 上下文预算（按百分比分配，取代固定轮数）──
    int model_context_tokens{131072};   // 模型窗口大小
    // 原文缓冲比例。天花板是 TokenBudget::kRawBufferMaxRatio（50%），
    // 写超过也按 50% 算；想要更省可以往下调。
    double raw_context_ratio{0.40};     // 原文可占比例（上限 50%）
    double summary_ratio{0.25};         // 摘要可占余下部分的比例
    // 没有条数上限：记忆只增不减，超预算时压成摘要 + 落盘，不删除。

    std::string notes_dir{"data/memory/notes"};
    std::string db_path{"data/memory/index.db"};
  };

  MemorySystem(Config cfg, fish::core::IEmbeddingModel* embedding);
  fish::core::Error init();
  fish::core::Error record(const std::string& conv_id, const Message& user,
                           const Message& assistant);
  // 取上下文。
  //
  // 没有 query 参数了（缺陷 7）：这个参数从来没有被用过，是死参数。
  // L3 的自动召回**不在**这里做 —— 当前只有模型主动调 search_memory
  // 工具才会召回 L3；要改成自动召回是另一个更大的改动。
  fish::core::Result<MemoryContext> gather(const std::string& conv_id);
  fish::core::Result<ConsolidateReport> maybe_consolidate(const std::string& conv_id,
                                                          Consolidator& consolidator);
  fish::core::Result<ConsolidateReport> force_consolidate(const std::string& conv_id,
                                                          Consolidator& consolidator);
  [[nodiscard]] int round_count(const std::string& conv_id) const;
  // 当前本地日期 yyyy-mm-dd（供外部做日期比较，也便于测试）
  [[nodiscard]] std::string today_public() const { return today(); }
  void set_summary(const std::string& conv_id, std::string summary);

  // 是否超预算（按 token，不按轮数）
  [[nodiscard]] bool needs_compaction(const std::string& conv_id) const;
  // 超预算时：最早部分 → 摘要 + 落盘
  core::Result<ConsolidateReport> compact(const std::string& conv_id,
                                          Consolidator& consolidator);
  // ── 供模型工具调用的检索 ──
  // 模型自己决定查什么；返回命中的历史片段。
  //
  // conv_id **必填**：检索按会话隔离。没有 conv_id 就无法保证不跨会话
  // 泄漏（A 群的事实被 B 群召回是隐私问题，见缺陷 2）。
  struct SearchHit {
    std::string kind;     // "fact" / "episode"
    std::string text;
    double score{0.0};
  };
  core::Result<std::vector<SearchHit>> search(const std::string& conv_id,
                                               const std::string& query, int limit);

  // ── 索引重建（缺陷 3）──
  // 以旁路 Markdown 为准重建索引：先清空索引，再从 facts/episodes
  // 重新灌入。可由运维/控制面显式调用。
  //
  // 返回值：重建后索引里的条目数。
  core::Result<std::size_t> rebuild_index();

  // 记住一个网络热梗（模型学到新梗时调用）。
  //
  // 为什么要有这个：热梗写回**本地库**只解决「下次查得到」，
  // 但模型在对话里并不会因此知道「这个人/这个群在用这个梗」。
  // 落一条 trait + 一条 fact 到**当前会话**的记忆里，
  // 模型下次被召回时就知道这个梗的来历，也可以顺着用。
  //
  // 语义：
  //   - trait pattern = 梗名本身，
  //     同一个梗重复出现时 **evidence 累加**（不是覆盖）
  //   - fact content  = "对方在用网络梗「xx」"
  //     id 用内容哈希，重复写入幂等，不会刷屏
  //
  // **不记梗的释义**：释义是关于世界的知识，已经在 memes.tsv 里（全局共享）；
  // facts 是「关于用户」的画像，把释义写进去会污染画像，
  // 而且 facts 按会话隔离，同一个梗换个会话就查不到了。
  //
  // 失败只打 warn：记不住一个梗不该让主链路失败。
  void remember_meme(const std::string& conv_id, const std::string& term,
                     const std::string& meaning = {});

  [[nodiscard]] NotesStore& notes() { return notes_; }
  [[nodiscard]] IndexStore& index() { return index_; }

 private:
  std::string today() const;

  // 统计旁路 Markdown（真源）里的条目总数：facts + episodes。
  // 启动时用来判断索引是否与真源一致（见 init() 与 rebuild_index()）。
  [[nodiscard]] std::size_t count_source_items() const;

  Config cfg_;
  fish::core::IEmbeddingModel* embedding_;
  NotesStore notes_;
  IndexStore index_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<WorkingMemory>> working_;
  std::unordered_map<std::string, std::string> summaries_;  // 滚动摘要
};

}  // namespace fish::memory

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::memory {

std::size_t estimate_tokens(const std::string& text) {
  if (text.empty()) return 0;
  std::size_t ascii = 0;
  std::size_t wide = 0;
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      ++ascii;
      ++i;
    } else {
      std::size_t len = 1;
      if ((c & 0xE0) == 0xC0) len = 2;
      else if ((c & 0xF0) == 0xE0) len = 3;
      else if ((c & 0xF8) == 0xF0) len = 4;
      i += len;
      ++wide;
    }
  }
  const std::size_t ascii_tokens = ascii / 4 + (ascii % 4 ? 1 : 0);
  return wide + ascii_tokens;
}

TokenBudget TokenBudget::make(int model_context_tokens, double raw_ratio,
                              double summary_ratio) {
  TokenBudget b;
  if (model_context_tokens <= 0) return b;
  b.total = static_cast<std::size_t>(model_context_tokens);

  // 原文缓冲比例：配置值可低不可高，天花板固定为 kRawBufferMaxRatio。
  //
  // 下限保护（缺陷 4）：raw_ratio 配得过小会让**每轮**都触发压缩，
  // 每轮都烧一次 LLM 巩固，且压缩比太狠导致摘要失真。
  // 低于 kRawBufferMinRatio 时抬到下限，并打 warn 日志说明
  // 「配置过于激进，已按下限处理」——不静默照做。
  double rr = raw_ratio;
  if (!(rr > 0.0)) {
    rr = kRawBufferDefaultRatio;
  } else if (rr < kRawBufferMinRatio) {
    // 只告警一次：TokenBudget::make 每轮都会被调用，
    // 每次都打一条 warn 会把日志刷爆。
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                            "raw_context_ratio_too_low_clamped")
          .field("configured", rr)
          .field("min", kRawBufferMinRatio)
          .field("detail", "配置过于激进，已按下限处理");
    }
    rr = kRawBufferMinRatio;
  }
  if (rr > kRawBufferMaxRatio) rr = kRawBufferMaxRatio;
  b.raw_budget = static_cast<std::size_t>(static_cast<double>(b.total) * rr);

  const std::size_t rest = b.total > b.raw_budget ? b.total - b.raw_budget : 0;
  double sr = summary_ratio;
  if (!(sr > 0.0)) sr = 0.0;
  if (sr > 0.9) sr = 0.9;
  b.summary_budget = static_cast<std::size_t>(static_cast<double>(rest) * sr);
  b.reserve = rest > b.summary_budget ? rest - b.summary_budget : 0;
  return b;
}

}  // namespace fish::memory


namespace fs = std::filesystem;

namespace fish::memory {

using fish::core::Error;
using fish::core::ErrorCode;
using fish::core::Result;

namespace {

std::string serialize_vec(const std::vector<float>& v) {
  std::string out(v.size() * sizeof(float), '\0');
  if (!v.empty()) std::memcpy(out.data(), v.data(), out.size());
  return out;
}

std::vector<float> deserialize_vec(const void* data, int bytes) {
  std::vector<float> out(static_cast<std::size_t>(bytes) / sizeof(float));
  if (bytes > 0) std::memcpy(out.data(), data, static_cast<std::size_t>(bytes));
  return out;
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.empty() || b.empty() || a.size() != b.size()) return 0.0;
  double dot = 0;
  double na = 0;
  double nb = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * b[i];
    na += static_cast<double>(a[i]) * a[i];
    nb += static_cast<double>(b[i]) * b[i];
  }
  if (na <= 0 || nb <= 0) return 0.0;
  return dot / (std::sqrt(na) * std::sqrt(nb));
}

// 表是否存在（用于旧库迁移检测）。
bool table_exists(sqlite3* db, const char* name) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
  const bool found = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return found;
}

// 列是否存在（用于旧库迁移检测）。
//
// 注意：PRAGMA table_info(...) 的参数**不能**是绑定参数 ——
// PRAGMA 里只接受字面量（字符串/标识符），传 ? 会报错或返回空结果。
// 所以这里把表名拼进 SQL。表名来自本代码内部（固定两个字面量），
// 不存在注入面。
bool column_exists(sqlite3* db, const char* table, const char* column) {
  const std::string sql = "PRAGMA table_info(" + std::string(table) + ");";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
  bool found = false;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* cn = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (cn != nullptr && std::strcmp(cn, column) == 0) {
      found = true;
      break;
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

bool parse_fact_line(const std::string& line, Fact* out) {
  static const std::regex re(
      R"(^\s*-\s*\[id:([^\]]+)\]\s*(.+?)(?:\s*\(置信度\s*([0-9.]+)\s*,\s*([0-9-]+)\))?\s*$)");
  std::smatch m;
  if (!std::regex_match(line, m, re)) return false;

  out->id = m[1];
  std::string content = m[2];
  const auto arrow = content.find("→ 被 ");
  if (arrow != std::string::npos) {
    std::string tail = content.substr(arrow + 5);
    const std::string suffix = "取代";
    while (!tail.empty() && (tail.back() == ' ' || tail.back() == '\t')) tail.pop_back();
    while (tail.size() >= suffix.size() &&
           tail.compare(tail.size() - suffix.size(), suffix.size(), suffix) == 0) {
      tail.erase(tail.size() - suffix.size());
    }
    while (!tail.empty() && (tail.back() == ' ' || tail.back() == '\t')) tail.pop_back();
    out->superseded_by = tail;
    content = content.substr(0, arrow);
    content.erase(std::remove(content.begin(), content.end(), '~'), content.end());
  }
  out->content = content;
  out->confidence = m[3].matched ? std::stod(m[3]) : 0.8;
  out->created = m[4].matched ? std::string(m[4]) : "";
  return true;
}

std::string json_extract(const std::string& raw) {
  const auto fence = raw.find("```");
  if (fence != std::string::npos) {
    const auto start = raw.find('\n', fence);
    const auto end = raw.find("```", start);
    if (start != std::string::npos && end != std::string::npos) {
      return raw.substr(start + 1, end - start - 1);
    }
  }
  const auto lb = raw.find('{');
  const auto rb = raw.rfind('}');
  if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
    return raw.substr(lb, rb - lb + 1);
  }
  return raw;
}

// ── episode 正文的编码/解码（缺陷 1）──────────────────────
//
// 为什么需要这一层：compact() 落盘的是**整段多行 transcript**，
// 而 episodes/*.md 的格式是「一条 episode 一行」。旧实现把多行正文
// 直接写进文件，load_episodes() 又按行解析 → 只能读回第一行，
// 其余对话既不解析也不报错，静默丢失（实测：写 367 字节，读回 24 字节）。
//
// 方案：**转义**。正文里的换行写成字面量 "\n"（反斜杠 + n），
// 反斜杠本身写成 "\\"。
//
// 为什么选转义而不是「给多行条目加起止标记」：
//   - 文件格式不变：一条 episode 一行。文件头有说明、用户可能手工编辑，
//     引入标记语法反而让人工编辑更容易出错；
//   - 标记语法会和正文内容抢语法（正文里出现标记行就会截断条目）；
//   - 旧数据（单行条目）原样就能读，不受影响。
//
// 代价（已知且接受）：正文里本来就写着「反斜杠+n」两个字符的旧数据，
// 解码后会被当成换行。聊天正文几乎不会出现反斜杠，概率可忽略。
std::string encode_episode_text(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (const char c : s) {
    if (c == '\\') {
      out += "\\\\";
      continue;
    }
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    if (c == '\r') {
      out += "\\r";
      continue;
    }
    out += c;
  }
  return out;
}

std::string decode_episode_text(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c != '\\' || i + 1 >= s.size()) {
      out += c;
      continue;
    }
    const char n = s[i + 1];
    if (n == 'n') {
      out += '\n';
      ++i;
      continue;
    }
    if (n == 'r') {
      out += '\r';
      ++i;
      continue;
    }
    if (n == '\\') {
      out += '\\';
      ++i;
      continue;
    }
    // 未知转义：原样保留（两个字符都写回去）
    out += c;
  }
  return out;
}

// SQL LIKE 的通配符转义：\ % _ 都要处理。
//
// 不转义的话，模型或用户传进来的 '%' 会退化成「匹配全部」，
// '_' 会变成单字符通配 —— 关键词路就变成了全表扫描。
// 转义符用 '\'，所以 SQL 里要写 ESCAPE '\'。
std::string escape_like(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    if (c == '\\' || c == '%' || c == '_') out += '\\';
    out += c;
  }
  return out;
}

// 关键词路的相关度打分（缺陷 6）。
//
// 只用于**路内排序**：命中位置越靠前、命中次数越多，分越高。
// Hit.score 本身仍然是 1.0（= 精确命中），语义不变。
double keyword_relevance(const std::string& text, const std::string& keyword) {
  if (keyword.empty()) return 0.0;
  // ASCII 大小写不敏感（与 SQLite LIKE 的默认行为一致）
  const auto lower = [](char c) -> char {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  };
  const std::string hay = [&] {
    std::string h = text;
    for (auto& c : h) c = lower(c);
    return h;
  }();
  const std::string needle = [&] {
    std::string n = keyword;
    for (auto& c : n) c = lower(c);
    return n;
  }();

  std::size_t count = 0;
  std::size_t first = std::string::npos;
  std::size_t pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    if (count == 0) first = pos;
    ++count;
    pos += needle.size();
  }
  if (count == 0) return 0.0;
  // 命中次数越多越相关；位置越靠前越相关（按 64 字符一个衰减档）
  const double position_bonus = 1.0 / (1.0 + static_cast<double>(first) / 64.0);
  return static_cast<double>(count) + position_bonus;
}

std::string render_transcript(const std::vector<Message>& msgs) {
  std::ostringstream oss;
  for (const auto& m : msgs) {
    oss << (m.role == "user" ? "对方: " : "我: ") << m.text << "\n";
  }
  return oss.str();
}

// 剥掉正文里残留的服务端工具留痕标记。
//
// 旧格式：正文末尾接一段 "\n\x01stc:<json>\x01ann:<json>\x01"。
// 现在正常路径不再产生这种正文（改用 Message 的两个独立字段），
// 但历史数据里还有，读取/落盘时必须剥掉，否则会：
//   - 进 episodes.md（用户可见的垃圾）
//   - 进 SQLite 向量索引（召回出来的片段带乱码）
//   - 被喂给 consolidate 的 LLM（模型看到控制字符）
//
// 做法：从第一个 "\x01stc:" 起全部截掉（标记段后面没有正文了）。
void strip_tool_markers(Message& m) {
  static const std::string kMark = "\x01stc:";
  const auto pos = m.text.find(kMark);
  if (pos == std::string::npos) return;
  m.text.resize(pos);
  // 顺手把尾部空白去掉
  while (!m.text.empty() && (m.text.back() == '\n' || m.text.back() == '\r' ||
                             m.text.back() == ' ' || m.text.back() == '\t')) {
    m.text.pop_back();
  }
}

constexpr const char* kConsolidatePrompt =
    "你是一个记忆整理器。输入是一段对话，提取三类信息。\n"
    "规则：只提取真的说过的内容，绝不编造；客套话与重复内容忽略；"
    "事实写成简洁陈述句；情景摘要一句话不超过 40 字。\n"
    "输出严格 JSON，不要解释：\n"
    R"({"episodes":[{"summary":"...","importance":0.5}],)"
    R"("facts":[{"subject":"用户","content":"...","confidence":0.9}],)"
    R"("traits":[{"pattern":"...","evidence":1}]})"
    "\n没有值得记的内容就三个数组都给空。";

}  // namespace

// ── WorkingMemory ───────────────────────────────────────
WorkingMemory::WorkingMemory(int max_rounds)
    : max_rounds_(max_rounds) {}   // <=0 表示不按轮数截断

void WorkingMemory::push(const Message& user, const Message& assistant) {
  std::lock_guard<std::mutex> lock(mu_);
  msgs_.push_back(user);
  msgs_.push_back(assistant);
  if (max_rounds_ > 0) {
    const std::size_t limit = static_cast<std::size_t>(max_rounds_) * 2;
    while (msgs_.size() > limit) msgs_.pop_front();
  }
}

std::vector<Message> WorkingMemory::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return std::vector<Message>(msgs_.begin(), msgs_.end());
}

int WorkingMemory::round_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return static_cast<int>(msgs_.size() / 2);
}

std::vector<Message> WorkingMemory::take_all() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Message> out(msgs_.begin(), msgs_.end());
  msgs_.clear();
  return out;
}

void WorkingMemory::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  msgs_.clear();
}

void WorkingMemory::restore(const std::vector<Message>& msgs) {
  std::lock_guard<std::mutex> lock(mu_);
  msgs_.assign(msgs.begin(), msgs.end());
  if (max_rounds_ > 0) {
    const std::size_t limit = static_cast<std::size_t>(max_rounds_) * 2;
    while (msgs_.size() > limit) msgs_.pop_front();
  }
}

// ── NotesStore ──────────────────────────────────────────
NotesStore::NotesStore(std::string notes_dir) : dir_(std::move(notes_dir)) {}

Error NotesStore::ensure_dirs() const {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  if (ec) return Error::make(ErrorCode::kInternal, "mkdir:" + ec.message());
  migrate_legacy_dirs();
  return Error::none();
}

void NotesStore::migrate_legacy_dirs() const {
  // 旧编码 → 新编码的一次性迁移。
  //
  // 旧编码把 ':' 一律换成 '_'，于是群会话 "g:<openid>" 的目录名是
  // "g_<openid>"；新编码用 "__COLON__" 标记，目录名是
  // "g__COLON__<openid>"。改编码后旧目录读不到 → 已有的事实/情景记忆
  // 失效、索引重建扫不到任何东西。
  //
  // 做法：启动时扫一遍 notes/{group,user}/，把旧名目录 rename 成新名。
  // 群里 conv_id 必然以 "g:" 开头，所以旧名必然以 "g_" 开头，
  // 还原规则是确定的；私聊 conv_id 就是 openid 本身（不含 ':'），
  // 新旧同名，不需要迁移。
  //
  // 迁移失败不阻塞启动：旧目录还在，数据没丢，只是仍走旧名读取。
  std::error_code ec;
  for (const char* kind : {"group", "user"}) {
    const fs::path base = fs::path(dir_) / kind;
    if (!fs::exists(base, ec)) continue;
    std::vector<fs::path> names;
    for (const auto& entry : fs::directory_iterator(base, ec)) {
      if (!entry.is_directory()) continue;
      names.push_back(entry.path());
    }
    for (const auto& p : names) {
      const std::string old_name = p.filename().string();
      // 只迁旧名：以 "g_" 开头（= 编码过的 "g:"）**且**不含新标记。
      //
      // 注意新名 "g__COLON__<openid>" 同样以 "g_" 开头，所以必须额外
      // 排除含 "__COLON__" 的目录 —— 否则会被二次迁移成
      // "g__COLON____COLON__<openid>"。
      if (std::string(kind) != "group") continue;         // 私聊新旧同名
      if (old_name.find("__COLON__") != std::string::npos) continue;  // 已是新名
      if (old_name.rfind("g_", 0) != 0) continue;         // 不是旧名
      const std::string new_name = "g__COLON__" + old_name.substr(2);
      const fs::path target = base / new_name;
      if (fs::exists(target, ec)) continue;   // 目标已存在，保留两边不覆盖
      fs::rename(p, target, ec);
      if (ec) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                              "session_dir_migrate_failed")
            .field("from", old_name)
            .field("to", new_name)
            .field("detail", ec.message());
      }
    }
  }
}

std::string NotesStore::conv_id_from_dir(const std::string& kind,
                                        const std::string& dir_name) {
  static const std::string kColonMark = "__COLON__";
  // 新编码：目录名里带 "__COLON__"
  if (dir_name.find(kColonMark) != std::string::npos) {
    std::string restored;
    restored.reserve(dir_name.size());
    std::size_t pos = 0;
    while (pos < dir_name.size()) {
      if (dir_name.compare(pos, kColonMark.size(), kColonMark) == 0) {
        restored += ':';
        pos += kColonMark.size();
        continue;
      }
      restored += dir_name[pos];
      ++pos;
    }
    // 群会话：目录名存的是去掉 "g:" 前缀的 openid，这里补回来。
    if (kind == "group" && restored.rfind("g:", 0) != 0) restored = "g:" + restored;
    return restored;
  }
  // 旧编码：群目录名必然以 "g_" 开头（那是编码后的 "g:"），
  // 还原成 "g:"；私聊目录名就是 openid 本身。
  if (kind == "group" && dir_name.rfind("g_", 0) == 0) {
    return "g:" + dir_name.substr(2);
  }
  return dir_name;
}

std::string NotesStore::session_dir(const std::string& conv_id) const {
  // 会话 id 直接做目录名不安全（可能含 / : 等），只保留 [A-Za-z0-9-_]。
  //
  // 但 ':' 必须是**可逆**的：群会话键是 "g:<openid>"，索引重建时要从
  // 目录名反推回 conv_id。以前 ':' 被换成 '_'，反推时又拼一个 "g:"，
  // 得到 "g:g_<openid>" → 读不到任何文件 → 索引重建静默为空。
  //
  // 现在用 "__COLON__" 标记 ':'（openid 是 base64url 风格，不含下划线，
  // 不会撞车），反推时还原。
  std::string safe;
  safe.reserve(conv_id.size());
  for (const char c : conv_id) {
    if (c == ':') {
      safe += "__COLON__";
      continue;
    }
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    safe += ok ? c : '_';
  }
  if (safe.empty()) safe = "default";
  // 前缀区分群聊/私聊，避免 openid 恰好同名的极端情况
  const std::string kind = (conv_id.rfind("g:", 0) == 0) ? "group" : "user";
  return dir_ + "/" + kind + "/" + safe;
}

std::string NotesStore::session_dir_legacy(const std::string& conv_id) const {
  // 旧编码：':' 一律换成 '_'。保留是为了读旧数据（见 load_* 的兼容分支）。
  std::string safe;
  safe.reserve(conv_id.size());
  for (const char c : conv_id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    safe += ok ? c : '_';
  }
  if (safe.empty()) safe = "default";
  const std::string kind = (conv_id.rfind("g:", 0) == 0) ? "group" : "user";
  return dir_ + "/" + kind + "/" + safe;
}

Error NotesStore::ensure_session_dir(const std::string& conv_id) const {
  std::error_code ec;
  fs::create_directories(session_dir(conv_id) + "/episodes", ec);
  if (ec) return Error::make(ErrorCode::kInternal, "mkdir_session:" + ec.message());
  return Error::none();
}

std::string NotesStore::session_dir_read(const std::string& conv_id) const {
  const std::string fresh = session_dir(conv_id);
  std::error_code ec;
  if (fs::exists(fresh, ec)) return fresh;
  const std::string old = session_dir_legacy(conv_id);
  if (old != fresh && fs::exists(old, ec)) return old;
  return fresh;
}

Result<std::vector<Fact>> NotesStore::load_facts(const std::string& conv_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream in(session_dir_read(conv_id) + "/facts.md");
  std::vector<Fact> out;
  if (!in) return Result<std::vector<Fact>>::ok({});

  std::string line;
  std::string section;
  while (std::getline(in, line)) {
    if (line.rfind("## ", 0) == 0) {
      section = line.substr(3);
      while (!section.empty() && (section.back() == ' ' || section.back() == '\r')) {
        section.pop_back();
      }
      continue;
    }
    Fact f;
    if (parse_fact_line(line, &f)) {
      if (!section.empty()) f.subject = section;
      out.push_back(std::move(f));
    }
  }
  return Result<std::vector<Fact>>::ok(std::move(out));
}

Error NotesStore::append_fact(const std::string& conv_id, const Fact& f) const {
  ensure_session_dir(conv_id);
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  const std::string sdir = session_dir(conv_id);
  fs::create_directories(sdir, ec);

  // 读现有内容（已持锁，内联读取避免重入）。
  //
  // 读源用 session_dir_read：迁移没跑成功时旧数据还在旧目录里，
  // 这里必须从旧目录读，否则会把已有事实当成空表重写（丢数据）。
  const std::string read_dir = session_dir_read(conv_id);
  std::vector<Fact> all;
  {
    std::ifstream in(read_dir + "/facts.md");
    std::string line;
    std::string section;
    while (std::getline(in, line)) {
      if (line.rfind("## ", 0) == 0) {
        section = line.substr(3);
        while (!section.empty() && (section.back() == ' ' || section.back() == '\r')) {
          section.pop_back();
        }
        continue;
      }
      Fact x;
      if (parse_fact_line(line, &x)) {
        if (!section.empty()) x.subject = section;
        all.push_back(std::move(x));
      }
    }
  }
  all.push_back(f);

  const std::string path = sdir + "/facts.md";
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return Error::make(ErrorCode::kInternal, "open_facts");
    out << "# 关于用户的长期记忆\n\n"
        << "<!-- 本文件由 super-feiyu 自动维护，可手工修正 -->\n\n";
    std::string cur;
    for (const auto& x : all) {
      if (x.subject != cur) {
        cur = x.subject;
        out << "## " << cur << "\n";
      }
      out << "- [id:" << x.id << "] " << x.content;
      if (!x.superseded_by.empty()) out << " → 被 " << x.superseded_by << " 取代";
      out << " (置信度 " << x.confidence << ", " << x.created << ")\n";
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) return Error::make(ErrorCode::kInternal, "rename_facts");
  return Error::none();
}

Error NotesStore::rewrite_facts(const std::string& conv_id,
                                const std::vector<Fact>& facts) const {
  ensure_session_dir(conv_id);
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  const std::string sdir = session_dir(conv_id);
  fs::create_directories(sdir, ec);

  const std::string path = sdir + "/facts.md";
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return Error::make(ErrorCode::kInternal, "open_facts_tmp");
    out << "# 关于用户的长期记忆\n\n"
        << "<!-- 本文件由 super-feiyu 自动维护，可手工修正 -->\n\n";
    std::string cur;
    for (const auto& x : facts) {
      if (x.subject != cur) {
        cur = x.subject;
        out << "## " << cur << "\n";
      }
      out << "- [id:" << x.id << "] " << x.content;
      if (!x.superseded_by.empty()) out << " → 被 " << x.superseded_by << " 取代";
      out << " (置信度 " << x.confidence << ", " << x.created << ")\n";
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) return Error::make(ErrorCode::kInternal, "rename_facts");
  return Error::none();
}

Error NotesStore::append_episode(const std::string& conv_id, const Episode& e) const {
  ensure_session_dir(conv_id);
  std::lock_guard<std::mutex> lock(mu_);
  std::string month = e.created.size() >= 7 ? e.created.substr(0, 7) : "unknown";

  std::error_code ec;
  const std::string sdir = session_dir(conv_id);
  fs::create_directories(sdir + "/episodes", ec);
  const std::string path = sdir + "/episodes/" + month + ".md";
  const bool fresh = !fs::exists(path);

  std::ofstream out(path, std::ios::app);
  if (!out) return Error::make(ErrorCode::kInternal, "open_episode");
  if (fresh) out << "# " << month << " 情景记忆\n\n";
  // 正文先转义成单行（换行 → 字面量 "\n"），保证「一条 episode 一行」
  // 的文件格式不变，读回来能 100% 还原（缺陷 1）。
  out << "- [id:" << e.id << "] " << encode_episode_text(e.summary) << " (重要度 "
      << e.importance << ", " << e.created << ")\n";
  return Error::none();
}

Error NotesStore::rewrite_episodes(const std::string& conv_id,
                                   const std::vector<Episode>& eps) const {
  ensure_session_dir(conv_id);
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  const std::string ep_dir = session_dir(conv_id) + "/episodes";
  fs::create_directories(ep_dir, ec);

  // 按月份分组
  std::map<std::string, std::vector<Episode>> by_month;
  for (const auto& e : eps) {
    const std::string month = e.created.size() >= 7 ? e.created.substr(0, 7) : "unknown";
    by_month[month].push_back(e);
  }

  // 清掉旧文件，再按分组写回（先写 tmp 再 rename，避免中途失败丢失）
  for (const auto& entry : fs::directory_iterator(ep_dir, ec)) {
    if (entry.is_regular_file() && entry.path().extension() == ".md") {
      fs::remove(entry.path(), ec);
    }
  }
  for (const auto& [month, list] : by_month) {
    const std::string path = ep_dir + "/" + month + ".md";
    const std::string tmp = path + ".tmp";
    {
      std::ofstream out(tmp, std::ios::trunc);
      if (!out) return Error::make(ErrorCode::kInternal, "open_episode_tmp");
      out << "# " << month << " 情景记忆\n\n";
      for (const auto& e : list) {
        // 与 append_episode 严格对称：写前转义（缺陷 1）。
        out << "- [id:" << e.id << "] " << encode_episode_text(e.summary) << " (重要度 "
            << e.importance << ", " << e.created << ")\n";
      }
    }
    fs::rename(tmp, path, ec);
    if (ec) return Error::make(ErrorCode::kInternal, "rename_episode");
  }
  return Error::none();
}

Result<std::vector<Episode>> NotesStore::load_episodes(const std::string& conv_id,
                                                      int max_months) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Episode> out;
  std::error_code ec;
  const fs::path ep_dir = session_dir_read(conv_id) + "/episodes";
  if (!fs::exists(ep_dir, ec)) return Result<std::vector<Episode>>::ok({});

  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(ep_dir, ec)) {
    if (entry.is_regular_file() && entry.path().extension() == ".md") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  if (static_cast<int>(files.size()) > max_months) {
    files.erase(files.begin(),
                files.begin() + static_cast<std::ptrdiff_t>(files.size() - max_months));
  }

  static const std::regex re(
      R"(^\s*-\s*\[id:([^\]]+)\]\s*(.+?)(?:\s*\(重要度\s*([0-9.]+)\s*,\s*([0-9-]+)\))?\s*$)");
  // 旧格式（缺陷 1 修复前写入的）：元数据**单独占一行**，
  // 正文是多行 transcript，元数据行形如 "  (重要度 0.5, 2026-11-15)"。
  static const std::regex meta_re(R"(^\s*\(重要度\s*([0-9.]+)\s*,\s*([0-9-]+)\)\s*$)");

  for (const auto& f : files) {
    std::ifstream in(f);
    std::string line;
    Episode cur;
    bool have = false;      // 是否正在攒一条 episode
    bool encoded = false;   // 正文是否是转义格式（新版写出的）
    while (std::getline(in, line)) {
      std::smatch m;
      if (std::regex_match(line, m, re)) {
        // 上一条没读完（文件被截断等异常）：先把已攒的收掉。
        if (have) out.push_back(cur);
        cur = Episode{};
        cur.id = m[1];
        cur.importance = m[3].matched ? std::stod(m[3]) : 0.5;
        cur.created = m[4].matched ? std::string(m[4]) : "";
        // 元数据与正文在同一行 → 新版（转义）格式 → 需要解码。
        // 元数据不在同一行 → 旧格式的正文，原样保留（不误解码）。
        encoded = m[4].matched;
        cur.summary = encoded ? decode_episode_text(m[2]) : std::string(m[2]);
        have = true;
        continue;
      }
      if (!have) continue;   // 文件标题等非条目行
      std::smatch mm;
      if (std::regex_match(line, mm, meta_re)) {
        // 旧格式：元数据单独一行，正文到这里才算完整。
        cur.importance = std::stod(mm[1]);
        cur.created = std::string(mm[2]);
        out.push_back(cur);
        have = false;
        encoded = false;
        continue;
      }
      // 旧格式的续行：原文追加（不转义、不解码）。
      if (!encoded) cur.summary += "\n" + line;
    }
    if (have) out.push_back(cur);
  }
  return Result<std::vector<Episode>>::ok(std::move(out));
}

Result<std::vector<Trait>> NotesStore::load_traits(const std::string& conv_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream in(session_dir_read(conv_id) + "/traits.md");
  std::vector<Trait> out;
  if (!in) return Result<std::vector<Trait>>::ok({});

  static const std::regex re(R"(^\s*-\s*(.+?)\s*\(证据\s*(\d+)\)\s*$)");
  std::string line;
  while (std::getline(in, line)) {
    std::smatch m;
    if (!std::regex_match(line, m, re)) continue;
    Trait t;
    t.pattern = m[1];
    t.evidence = std::stoi(m[2]);
    out.push_back(std::move(t));
  }
  return Result<std::vector<Trait>>::ok(std::move(out));
}

Error NotesStore::save_traits(const std::string& conv_id,
                              const std::vector<Trait>& traits) const {
  ensure_session_dir(conv_id);
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  const std::string sdir = session_dir(conv_id);
  fs::create_directories(sdir, ec);
  const std::string tmp = sdir + "/traits.md.tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return Error::make(ErrorCode::kInternal, "open_traits");
    out << "# 行为模式与偏好\n\n<!-- 证据 >= 3 才确立 -->\n\n";
    for (const auto& t : traits) {
      out << "- " << t.pattern << " (证据 " << t.evidence << ")\n";
    }
  }
  fs::rename(tmp, sdir + "/traits.md", ec);
  if (ec) return Error::make(ErrorCode::kInternal, "rename_traits");
  return Error::none();
}

// ── IndexStore ──────────────────────────────────────────
IndexStore::IndexStore(std::string db_path) : path_(std::move(db_path)) {}
IndexStore::~IndexStore() { close(); }

Error IndexStore::open() {
  sqlite3* db = nullptr;
  if (sqlite3_open(path_.c_str(), &db) != SQLITE_OK) {
    if (db != nullptr) sqlite3_close(db);
    return Error::make(ErrorCode::kInternal, "sqlite_open");
  }
  db_ = db;

  const char* pragmas[] = {"PRAGMA journal_mode=WAL;", "PRAGMA synchronous=NORMAL;",
                           "PRAGMA cache_size=-8000;", "PRAGMA busy_timeout=5000;"};
  for (const char* p : pragmas) {
    char* err = nullptr;
    sqlite3_exec(db, p, nullptr, nullptr, &err);
    if (err != nullptr) sqlite3_free(err);
  }

  // ── 旧库迁移（缺陷 2）──────────────────────────────────
  //
  // 老库的 items 表没有 conv_id，主键是 (kind, id)。
  // 新库必须有 conv_id：索引按会话隔离，否则 A 群的事实会被 B 群召回
  // （跨会话泄漏 = 隐私问题）。
  //
  // 为什么不是 ALTER TABLE ADD COLUMN：主键要变成 (conv_id, kind, id)，
  // SQLite 的 ALTER TABLE 不能改主键，只能重建表。
  // 迁移时把老条目 conv_id 置空 —— 无法归属的条目检索时**永不返回**，
  // 由 rebuild_index() 从旁路 Markdown 真源重新灌入。
  //
  // 注意用 INSERT OR REPLACE + GROUP BY 去重：老库可能已经有重复的
  // (kind, id) 行（比如当年主键约束没建成功），直接搬会撞 UNIQUE 约束
  // 导致迁移失败、整个索引打不开。
  if (table_exists(db, "items") && !column_exists(db, "items", "conv_id")) {
    char* err = nullptr;
    const char* migrate =
        "BEGIN;"
        "CREATE TABLE items_new (conv_id TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL,"
        " id TEXT NOT NULL, text TEXT NOT NULL, vec BLOB, dim INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY (conv_id, kind, id));"
        "INSERT OR REPLACE INTO items_new(conv_id,kind,id,text,vec,dim)"
        " SELECT '', kind, id, text, vec, dim FROM items"
        " GROUP BY kind, id;"
        "DROP TABLE items;"
        "ALTER TABLE items_new RENAME TO items;"
        "COMMIT;";
    if (sqlite3_exec(db, migrate, nullptr, nullptr, &err) != SQLITE_OK) {
      const std::string msg = err != nullptr ? err : "migrate";
      sqlite3_free(err);
      // 回滚：把事务撤掉，别留下半张表。
      char* rollback_err = nullptr;
      sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &rollback_err);
      if (rollback_err != nullptr) sqlite3_free(rollback_err);
      return Error::make(ErrorCode::kInternal, "sqlite_migrate:" + msg);
    }
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory", "index_schema_migrated")
        .field("detail", "items 表已加 conv_id；旧条目 conv_id 为空，检索不返回，"
                         "待 rebuild_index() 从旁路文件重建");
  }

  const char* ddl =
      "CREATE TABLE IF NOT EXISTS items (conv_id TEXT NOT NULL DEFAULT '',"
      " kind TEXT NOT NULL, id TEXT NOT NULL,"
      " text TEXT NOT NULL, vec BLOB, dim INTEGER NOT NULL DEFAULT 0,"
      " PRIMARY KEY (conv_id, kind, id));"
      "CREATE INDEX IF NOT EXISTS idx_kind ON items(kind);"
      "CREATE INDEX IF NOT EXISTS idx_conv_kind ON items(conv_id, kind);";
  char* err = nullptr;
  if (sqlite3_exec(db, ddl, nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string msg = err != nullptr ? err : "ddl";
    sqlite3_free(err);
    return Error::make(ErrorCode::kInternal, "sqlite_ddl:" + msg);
  }
  return Error::none();
}

Error IndexStore::close() {
  if (db_ != nullptr) {
    sqlite3_close(static_cast<sqlite3*>(db_));
    db_ = nullptr;
  }
  return Error::none();
}

Error IndexStore::upsert_item(const std::string& conv_id, const std::string& kind,
                              const std::string& id, const std::string& text,
                              const std::vector<float>& vec) {
  if (db_ == nullptr) return Error::make(ErrorCode::kInternal, "db_closed");
  std::lock_guard<std::mutex> lock(mu_);

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO items(conv_id,kind,id,text,vec,dim) VALUES(?,?,?,?,?,?) "
      "ON CONFLICT(conv_id,kind,id) DO UPDATE SET text=excluded.text,"
      "vec=excluded.vec,dim=excluded.dim;";
  if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error::make(ErrorCode::kInternal, "prepare");
  }

  const std::string blob = serialize_vec(vec);
  sqlite3_bind_text(stmt, 1, conv_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, text.c_str(), -1, SQLITE_TRANSIENT);
  if (blob.empty()) {
    sqlite3_bind_null(stmt, 5);
  } else {
    sqlite3_bind_blob(stmt, 5, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
  }
  sqlite3_bind_int(stmt, 6, static_cast<int>(vec.size()));

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return Error::make(ErrorCode::kInternal, "upsert");
  return Error::none();
}

Result<std::vector<IndexStore::Hit>> IndexStore::search(const std::vector<float>& query,
                                                        int top_k,
                                                        const std::string& conv_id,
                                                        const std::string& kind) const {
  if (db_ == nullptr) return Result<std::vector<Hit>>::fail(ErrorCode::kInternal, "db_closed");
  if (query.empty() || top_k <= 0) return Result<std::vector<Hit>>::ok({});
  std::lock_guard<std::mutex> lock(mu_);

  // conv_id 为空 = 不按会话过滤（仅内部诊断用）；
  // 否则严格按会话过滤（跨会话不泄漏）。
  //
 // 和 search_text 一样：conv_id 为空时也要排除 conv_id='' 的行
  // （老库迁移来的、无法归属的条目，任何路径都不返回）。
  const std::string sql =
      (conv_id.empty() && kind.empty())
          ? "SELECT kind,id,text,vec FROM items"
            " WHERE vec IS NOT NULL AND conv_id <> '';"
          : (conv_id.empty()
                 ? "SELECT kind,id,text,vec FROM items"
                   " WHERE vec IS NOT NULL AND conv_id <> '' AND kind=?;"
                 : (kind.empty()
                        ? "SELECT kind,id,text,vec FROM items WHERE vec IS NOT NULL"
                          " AND conv_id=?;"
                        : "SELECT kind,id,text,vec FROM items WHERE vec IS NOT NULL"
                          " AND conv_id=? AND kind=?;"));
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql.c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return Result<std::vector<Hit>>::fail(ErrorCode::kInternal, "prepare");
  }
  int bind_idx = 1;
  if (!conv_id.empty()) {
    sqlite3_bind_text(stmt, bind_idx++, conv_id.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!kind.empty()) {
    sqlite3_bind_text(stmt, bind_idx++, kind.c_str(), -1, SQLITE_TRANSIENT);
  }

  std::vector<std::pair<double, Hit>> scored;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const auto* i = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    const void* blob = sqlite3_column_blob(stmt, 3);
    const int bytes = sqlite3_column_bytes(stmt, 3);

    Hit h;
    h.kind = k != nullptr ? k : "";
    h.id = i != nullptr ? i : "";
    h.text = t != nullptr ? t : "";
    h.score = cosine(query, deserialize_vec(blob, bytes));
    if (h.score <= 0.0) continue;
    scored.emplace_back(h.score, std::move(h));
  }
  sqlite3_finalize(stmt);

  const std::size_t kk =
      std::min<std::size_t>(scored.size(), static_cast<std::size_t>(top_k));
  std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(kk),
                    scored.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<Hit> out;
  for (std::size_t i = 0; i < kk; ++i) out.push_back(std::move(scored[i].second));
  return Result<std::vector<Hit>>::ok(std::move(out));
}

Result<std::vector<IndexStore::Hit>> IndexStore::search_text(const std::string& conv_id,
                                                             const std::string& keyword,
                                                             int top_k) const {
  if (db_ == nullptr) return Result<std::vector<Hit>>::fail(ErrorCode::kInternal, "db_closed");
  if (keyword.empty() || top_k <= 0) return Result<std::vector<Hit>>::ok({});
  std::lock_guard<std::mutex> lock(mu_);

  // conv_id 为空 = 不按会话过滤（仅内部诊断用）；否则严格按会话过滤。
  //
  // 注意：conv_id 为空时也**必须排除 conv_id='' 的行**——
  // 那是老库迁移来的、无法归属到任何会话的条目。
  // 它们既不能被某个会话搜到，也不该被「不按会话过滤」的诊断查询带出来
  // （否则等于把不可归属的数据暴露给任何调用方）。
  const std::string sql =
      conv_id.empty()
          ? "SELECT kind,id,text FROM items WHERE conv_id <> '' AND text LIKE ? ESCAPE '\\';"
          : "SELECT kind,id,text FROM items WHERE conv_id=? AND text LIKE ? ESCAPE '\\';";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return Result<std::vector<Hit>>::fail(ErrorCode::kInternal, "prepare");
  }

  // 转义 LIKE 的通配符（缺陷 5）：不转义的话 '%' 会退化成「匹配全部」，
  // '_' 会变成单字符通配 —— 关键词路就变成了全表扫描。
  const std::string pattern = "%" + escape_like(keyword) + "%";
  int bind_idx = 1;
  if (!conv_id.empty()) {
    sqlite3_bind_text(stmt, bind_idx++, conv_id.c_str(), -1, SQLITE_TRANSIENT);
  }
  sqlite3_bind_text(stmt, bind_idx, pattern.c_str(), -1, SQLITE_TRANSIENT);

  // 不在 SQL 里 LIMIT：SQLite 没有内置相关度排序，LIMIT 截的是**插入序**
  // （缺陷 6）。先把候选全部取出来，在 C++ 侧按命中位置/词频打分排序，
  // 再取 top_k。
  std::vector<Hit> candidates;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    Hit h;
    const auto* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const auto* i = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    h.kind = k != nullptr ? k : "";
    h.id = i != nullptr ? i : "";
    h.text = t != nullptr ? t : "";
    h.score = 1.0;   // 语义：精确命中（关键词路恒为 1.0，不与向量路混用）
    candidates.push_back(std::move(h));
  }
  sqlite3_finalize(stmt);

  // 路内排序：命中位置越靠前、命中次数越多 → 相关度越高。
  std::vector<std::pair<double, Hit>> scored;
  scored.reserve(candidates.size());
  for (auto& h : candidates) {
    scored.emplace_back(keyword_relevance(h.text, keyword), std::move(h));
  }
  const std::size_t kk =
      std::min<std::size_t>(scored.size(), static_cast<std::size_t>(top_k));
  std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(kk),
                    scored.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<Hit> out;
  out.reserve(kk);
  for (std::size_t i = 0; i < kk; ++i) out.push_back(std::move(scored[i].second));
  return Result<std::vector<Hit>>::ok(std::move(out));
}

Error IndexStore::remove_item(const std::string& conv_id, const std::string& kind,
                              const std::string& id) {
  if (db_ == nullptr) return Error::make(ErrorCode::kInternal, "db_closed");
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "DELETE FROM items WHERE conv_id=? AND kind=? AND id=?;";
  if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error::make(ErrorCode::kInternal, "prepare_remove");
  }
  sqlite3_bind_text(stmt, 1, conv_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return Error::make(ErrorCode::kInternal, "remove_failed");
  return Error::none();
}

std::size_t IndexStore::item_count() const {
  if (db_ == nullptr) return 0;
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), "SELECT COUNT(*) FROM items;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    return 0;
  }
  std::size_t n = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = static_cast<std::size_t>(sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);
  return n;
}

std::size_t IndexStore::indexed_item_count() const {
  if (db_ == nullptr) return 0;
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_),
                         "SELECT COUNT(*) FROM items WHERE conv_id <> '';", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    return 0;
  }
  std::size_t n = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = static_cast<std::size_t>(sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);
  return n;
}

Error IndexStore::clear() {
  if (db_ == nullptr) return Error::make(ErrorCode::kInternal, "db_closed");
  std::lock_guard<std::mutex> lock(mu_);
  char* err = nullptr;
  if (sqlite3_exec(static_cast<sqlite3*>(db_), "DELETE FROM items;", nullptr, nullptr, &err) !=
      SQLITE_OK) {
    const std::string msg = err != nullptr ? err : "clear";
    sqlite3_free(err);
    return Error::make(ErrorCode::kInternal, "clear:" + msg);
  }
  return Error::none();
}

// ── Consolidator ────────────────────────────────────────
Consolidator::Consolidator(fish::core::IChatModel* chat, NotesStore* notes, IndexStore* index,
                           fish::core::IEmbeddingModel* embedding)
    : chat_(chat), notes_(notes), index_(index), embedding_(embedding) {}

Result<ConsolidateReport> Consolidator::consolidate(const std::string& conv_id,
                                                    const std::vector<Message>& transcript) {
  if (transcript.empty()) {
    ConsolidateReport r;
    r.skipped = true;
    r.reason = "empty_transcript";
    return Result<ConsolidateReport>::ok(r);
  }
  if (chat_ == nullptr || notes_ == nullptr) {
    return Result<ConsolidateReport>::fail(ErrorCode::kInternal, "not_wired");
  }

  fish::core::IChatModel::Request req;
  req.system_prompt = kConsolidatePrompt;
  fish::core::ContentBlock block;
  block.kind = fish::core::BlockKind::kText;
  block.text = render_transcript(transcript);
  req.user_blocks.push_back(std::move(block));
  req.max_tokens = 800;
  req.temperature = 0.3;

  auto resp = chat_->complete(req);
  if (!resp.ok()) {
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory", "consolidate_llm_failed")
        .field("detail", resp.error.detail);
    return Result<ConsolidateReport>::fail(resp.error.code, resp.error.detail);
  }

  const auto t = fish::core::Clock::to_time_t(transcript.front().at);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char date[32];
  std::snprintf(date, sizeof(date), "%04d-%02d-%02d", tm_buf.tm_year + 1900,
                tm_buf.tm_mon + 1, tm_buf.tm_mday);

  return apply_json(json_extract(resp.value->text), date, notes_, index_, embedding_,
                    conv_id);
}

Result<std::string> Consolidator::summarize(const std::vector<Message>& transcript) {
  if (transcript.empty()) return Result<std::string>::ok(std::string{});
  if (chat_ == nullptr) {
    return Result<std::string>::fail(ErrorCode::kInternal, "no_model");
  }
  std::ostringstream oss;
  for (const auto& m : transcript) {
    oss << (m.role == "user" ? "对方: " : "我: ") << m.text << "\n";
  }
  fish::core::IChatModel::Request req;
  req.system_prompt =
      "你把一段较早的聊天压成摘要，供之后接续对话时回忆。只保留聊过的话题、"
      "对方透露的事实、约定或未完成的事。条目式中文，不超过 200 字。";
  fish::core::ContentBlock b;
  b.kind = fish::core::BlockKind::kText;
  b.text = oss.str();
  req.user_blocks.push_back(std::move(b));
  req.max_tokens = 400;
  req.temperature = 0.2;
  auto r = chat_->complete(req);
  if (!r.ok()) return Result<std::string>::fail(r.error.code, r.error.detail);
  return Result<std::string>::ok(r.value->text);
}

Result<ConsolidateReport> Consolidator::apply_json(const std::string& json_text,
                                                   const std::string& today,
                                                   NotesStore* notes, IndexStore* index,
                                                   fish::core::IEmbeddingModel* embedding,
                                                   const std::string& conv_id) {
  nlohmann::json body;
  try {
    body = nlohmann::json::parse(json_text);
  } catch (const std::exception& e) {
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory", "consolidate_bad_json")
        .field("detail", e.what());
    return Result<ConsolidateReport>::fail(ErrorCode::kBadRequest, "bad_json");
  }

  ConsolidateReport report;
  std::size_t idx = 0;

  auto content_hash = [](const std::string& s) {
    std::size_t h = 1469598103934665603ULL;
    for (const char c : s) {
      h ^= static_cast<unsigned char>(c);
      h *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
  };
  std::vector<Episode> existing_eps;
  if (notes != nullptr) {
    auto ee = notes->load_episodes(conv_id);
    if (ee.ok()) existing_eps = std::move(*ee.value);
  }
  for (const auto& e : body.value("episodes", nlohmann::json::array())) {
    Episode ep;
    ep.summary = e.value("summary", "");
    ep.importance = e.value("importance", 0.5);
    ep.created = today;
    if (ep.summary.empty()) continue;
    ep.id = "e" + content_hash(ep.summary).substr(0, 12);
    const bool dup_ep = std::any_of(existing_eps.begin(), existing_eps.end(),
                                    [&](const Episode& x) { return x.summary == ep.summary; });
    if (dup_ep) continue;
    if (notes != nullptr) notes->append_episode(conv_id, ep);
    existing_eps.push_back(ep);
    if (index != nullptr && embedding != nullptr) {
      auto vec = embedding->embed(ep.summary);
      if (vec.ok()) index->upsert_item(conv_id, "episode", ep.id, ep.summary, *vec.value);
    }
    ++report.episodes_written;
  }

  idx = 0;

  // 幂等：同内容不重复写入；id 用内容哈希，保证跨批次稳定
  std::vector<Fact> existing_facts;
  if (notes != nullptr) {
    auto ef = notes->load_facts(conv_id);
    if (ef.ok()) existing_facts = std::move(*ef.value);
  }
  for (const auto& f : body.value("facts", nlohmann::json::array())) {
    Fact fact;
    fact.subject = f.value("subject", "用户");
    fact.content = f.value("content", "");
    fact.confidence = f.value("confidence", 0.8);
    fact.created = today;
    if (fact.content.empty()) continue;
    fact.id = "f" + content_hash(fact.content).substr(0, 12);
    const bool dup = std::any_of(existing_facts.begin(), existing_facts.end(),
                                 [&](const Fact& x) { return x.content == fact.content; });
    if (dup) continue;
    if (notes != nullptr) notes->append_fact(conv_id, fact);
    existing_facts.push_back(fact);
    if (index != nullptr && embedding != nullptr) {
      auto vec = embedding->embed(fact.content);
      if (vec.ok()) index->upsert_item(conv_id, "fact", fact.id, fact.content, *vec.value);
    }
    ++report.facts_written;
  }

  auto existing = notes != nullptr ? notes->load_traits(conv_id)
                                   : Result<std::vector<Trait>>::ok({});
  std::vector<Trait> traits = existing.ok() ? *existing.value : std::vector<Trait>{};

  for (const auto& t : body.value("traits", nlohmann::json::array())) {
    const std::string pattern = t.value("pattern", "");
    if (pattern.empty()) continue;
    auto it = std::find_if(traits.begin(), traits.end(),
                           [&](const Trait& x) { return x.pattern == pattern; });
    if (it != traits.end()) {
      it->evidence += t.value("evidence", 1);
    } else {
      traits.push_back(Trait{pattern, t.value("evidence", 1)});
    }
    ++report.traits_updated;
  }
  if (notes != nullptr && !traits.empty()) notes->save_traits(conv_id, traits);

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "memory", "consolidated")
      .field("episodes", report.episodes_written)
      .field("facts", report.facts_written)
      .field("traits", report.traits_updated);

  return Result<ConsolidateReport>::ok(report);
}

// ── MemorySystem ────────────────────────────────────────
MemorySystem::MemorySystem(Config cfg, fish::core::IEmbeddingModel* embedding)
    : cfg_(std::move(cfg)),
      embedding_(embedding),
      notes_(cfg_.notes_dir),
      index_(cfg_.db_path) {}

Error MemorySystem::init() {
  auto e = notes_.ensure_dirs();
  if (!e.ok()) return e;

  std::error_code ec;
  const auto parent = fs::path(cfg_.db_path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);

  auto idx = index_.open();
  if (!idx.ok()) return idx;

  // ── 配置合理性告警（缺陷 4）────────────────────────────
  //
  // model_context_tokens 配得比真实模型窗口大（例如配 1M、实际只有 128K）
  // 时，代码算出来的预算永远用不满 → 压缩永远不触发 → 攒到几十万 token
  // 才发一次请求，然后被上游直接拒掉。
  // 这里只提醒，不自动改：改配置是运维的决定。
  if (cfg_.model_context_tokens > TokenBudget::kLikelyOversizedContextTokens) {
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                          "model_context_tokens_looks_oversized")
        .field("configured", static_cast<long long>(cfg_.model_context_tokens))
        .field("threshold", static_cast<long long>(TokenBudget::kLikelyOversizedContextTokens))
        .field("detail", "配置的上下文窗口偏大，请核对模型真实窗口；"
                         "配得过大将导致压缩永不触发、请求被上游拒绝");
  }

  // ── 索引自愈（缺陷 3）──────────────────────────────────
  //
  // 旧逻辑：只有 item_count()==0 才重建。索引**部分损坏**（有条目但缺内容）
  // 时永远不会重建，缺的内容就永远召不回来。
  //
  // 新逻辑（启动时自检一次）：
  //   1) 索引为空但旁路文件有内容 → 重建（原来就有）
  //   2) 索引里有 conv_id 为空的条目（老库迁移来的，无法归属）→ 重建
  //      （这些条目检索时永不返回，等于死数据）
  //   3) 索引条目数少于旁路文件里的条目数 → 重建（真源为准）
  // 4) 显式重建入口 rebuild_index()，供运维/控制面调用
  //
  // 代价：第 3 条要遍历所有会话目录。只在启动时做一次，可以接受。
  if (embedding_ != nullptr) {
    const std::size_t indexed = index_.indexed_item_count();
    const std::size_t total = index_.item_count();
    const std::size_t source = count_source_items();
    // 有孤儿条目（conv_id 为空）说明是老库迁移来的，必须重建。
    const bool has_orphans = indexed != total;
    const bool missing = indexed < source;
    if (has_orphans || missing || (total == 0 && source > 0)) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                            "index_inconsistent_rebuilding")
          .field("indexed", static_cast<long long>(indexed))
          .field("total", static_cast<long long>(total))
          .field("source", static_cast<long long>(source));
      auto rb = rebuild_index();
      if (!rb.ok()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                              "index_rebuild_failed")
            .field("detail", rb.error.detail);
      }
    }
  }

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "memory", "init_ok")
      .field("db", cfg_.db_path)
      .field("notes", cfg_.notes_dir)
      .field("items", static_cast<long long>(index_.item_count()));
  return Error::none();
}

// 统计旁路 Markdown（真源）里的条目总数：facts + episodes。
//
// 用于启动时判断索引是否与真源一致（缺陷 3）。
// 只统计、不加载正文，比重建便宜得多。
std::size_t MemorySystem::count_source_items() const {
  std::size_t n = 0;
  std::error_code dir_ec;
  for (const auto& kind : {"group", "user"}) {
    const fs::path base = fs::path(cfg_.notes_dir) / kind;
    if (!fs::exists(base, dir_ec)) continue;
    for (const auto& entry : fs::directory_iterator(base, dir_ec)) {
      if (!entry.is_directory()) continue;
      const std::string conv_id = NotesStore::conv_id_from_dir(
          std::string(kind), entry.path().filename().string());
      auto facts = notes_.load_facts(conv_id);
      if (facts.ok()) n += facts.value->size();
      auto episodes = notes_.load_episodes(conv_id);
      if (episodes.ok()) n += episodes.value->size();
    }
  }
  return n;
}

// 以旁路 Markdown 为准重建索引（缺陷 3）。
//
// 步骤：清空索引 → 遍历所有会话目录 → 读 facts/episodes → 重新灌入。
//
// 为什么「以旁路为准」：旁路 Markdown 是权威源（用户可以手工改），
// SQLite 只是**可重建的派生索引**。索引坏了就重建，不修索引。
//
// 关于「不阻塞服务」：重建会调用 embedding 模型（可能较慢），
// 但它不持有 MemorySystem::mu_（会话锁），只持有 IndexStore 自己的锁，
// 因此其它会话的检索最多短暂等待，不会被整体卡死。
// 如果要完全异步，应该由调用方放到后台线程里跑 rebuild_index()。
Result<std::size_t> MemorySystem::rebuild_index() {
  if (embedding_ == nullptr) {
    return Result<std::size_t>::fail(ErrorCode::kUnsupported, "no_embedding");
  }
  auto cleared = index_.clear();
  if (!cleared.ok()) return Result<std::size_t>::fail(cleared.code, cleared.detail);

  std::size_t rebuilt = 0;
  std::error_code dir_ec;
  for (const auto& kind : {"group", "user"}) {
    const fs::path base = fs::path(cfg_.notes_dir) / kind;
    if (!fs::exists(base, dir_ec)) continue;
    for (const auto& entry : fs::directory_iterator(base, dir_ec)) {
      if (!entry.is_directory()) continue;
      // 目录名是脱敏后的会话 id，重建时用它反推 conv_id 去读文件。
      //
      // 反推必须**可逆**：群会话键是 "g:<openid>"，目录名（新编码）
      // 是 "g__COLON__<openid>"，还原回来是 "g:<openid>"。
      const std::string conv_id =
          NotesStore::conv_id_from_dir(std::string(kind), entry.path().filename().string());

      auto facts = notes_.load_facts(conv_id);
      if (facts.ok()) {
        for (const auto& f : *facts.value) {
          auto vec = embedding_->embed(f.content);
          if (vec.ok()) {
            index_.upsert_item(conv_id, "fact", f.id, f.content, *vec.value);
            ++rebuilt;
          }
        }
      }
      auto episodes = notes_.load_episodes(conv_id);
      if (episodes.ok()) {
        for (const auto& ep : *episodes.value) {
          auto vec = embedding_->embed(ep.summary);
          if (vec.ok()) {
            index_.upsert_item(conv_id, "episode", ep.id, ep.summary, *vec.value);
            ++rebuilt;
          }
        }
      }
    }
  }
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "memory", "index_rebuilt")
      .field("items", static_cast<long long>(rebuilt));
  return Result<std::size_t>::ok(rebuilt);
}


std::string MemorySystem::today() const {
  const auto t = fish::core::Clock::to_time_t(fish::core::Clock::now());
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm_buf.tm_year + 1900,
                tm_buf.tm_mon + 1, tm_buf.tm_mday);
  return buf;
}

Error MemorySystem::record(const std::string& conv_id, const Message& user,
                           const Message& assistant) {
  std::lock_guard<std::mutex> lock(mu_);
  auto& wm = working_[conv_id];
  if (!wm) wm = std::make_unique<WorkingMemory>(0);   // 0 = 不按轮数截断
  // 兜底：正文里的服务端工具标记必须在进记忆前剥掉。
  //
  // 现在编排层已经改用 server_tool_calls_json / annotations_json 两个
  // 独立字段，正常路径下正文是干净的。但历史数据（旧版本写入的
  // "\x01stc:...\x01ann:...\x01"）或别的调用方塞进来的正文仍可能带标记，
  // 留着就会进 episodes.md、进向量索引、还被喂给 consolidate 的 LLM。
  //
  // 这里统一在入口剥一次：宁可在记忆层做这个保证，也不要依赖每个
  // 调用方都记得清理。
  Message u = user;
  Message a = assistant;
  strip_tool_markers(u);
  strip_tool_markers(a);
  wm->push(u, a);
  return Error::none();
}

bool MemorySystem::needs_compaction(const std::string& conv_id) const {
  std::vector<Message> all;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = working_.find(conv_id);
    if (it == working_.end()) return false;
    all = it->second->snapshot();
  }
  const auto budget = TokenBudget::make(cfg_.model_context_tokens, cfg_.raw_context_ratio,
                                        cfg_.summary_ratio);
  if (budget.raw_budget == 0) return false;

  std::size_t total = 0;
  for (const auto& m : all) total += estimate_tokens(m.text) + 8;
  return total > budget.raw_budget;
}

Result<ConsolidateReport> MemorySystem::compact(const std::string& conv_id,
                                                Consolidator& consolidator) {
  std::vector<Message> all;
  std::string old_summary;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = working_.find(conv_id);
    if (it == working_.end() || it->second->round_count() == 0) {
      ConsolidateReport r;
      r.skipped = true;
      r.reason = "no_working_memory";
      return Result<ConsolidateReport>::ok(r);
    }
    all = it->second->snapshot();
    auto sit = summaries_.find(conv_id);
    if (sit != summaries_.end()) old_summary = sit->second;
  }

  const auto budget = TokenBudget::make(cfg_.model_context_tokens, cfg_.raw_context_ratio,
                                        cfg_.summary_ratio);

  // 从最新往回收，留下能放进预算的部分；更早的进入压缩集合
  std::size_t used = 0;
  std::size_t keep_from = all.size();
  for (std::size_t i = all.size(); i > 0; --i) {
    const std::size_t t = estimate_tokens(all[i - 1].text) + 8;
    if (budget.raw_budget > 0 && used + t > budget.raw_budget && keep_from < all.size()) break;
    used += t;
    keep_from = i - 1;
  }

  if (keep_from == 0) {
    ConsolidateReport r;
    r.skipped = true;
    r.reason = "under_budget";
    return Result<ConsolidateReport>::ok(r);
  }

  std::vector<Message> to_compact(all.begin(),
                                  all.begin() + static_cast<std::ptrdiff_t>(keep_from));
  std::vector<Message> to_keep(all.begin() + static_cast<std::ptrdiff_t>(keep_from),
                               all.end());

  // 生成滚动摘要（把旧摘要一起喂进去，保证连贯）
  std::vector<Message> sum_input;
  if (!old_summary.empty()) {
    Message prev;
    prev.role = "assistant";
    prev.text = "[更早的摘要] " + old_summary;
    sum_input.push_back(std::move(prev));
  }
  sum_input.insert(sum_input.end(), to_compact.begin(), to_compact.end());

  std::string new_summary = old_summary;
  if (auto sr = consolidator.summarize(sum_input); sr.ok() && !sr.value->empty()) {
    new_summary = std::move(*sr.value);
  } else {
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory", "summarize_failed");
  }

  // 原文落盘为情景记忆（不丢），并建索引供将来检索
  std::string transcript_text;
  for (const auto& m : to_compact) {
    transcript_text += (m.role == "user" ? "对方: " : "我: ") + m.text + "\n";
  }
  if (!transcript_text.empty()) {
    Episode ep;
    ep.id = "e" + std::to_string(
                      std::hash<std::string>{}(transcript_text) & 0xFFFFFFFFFFFFULL);
    ep.summary = transcript_text;
    ep.created = today();
    notes_.append_episode(conv_id, ep);
    if (embedding_ != nullptr) {
      if (auto v = embedding_->embed(transcript_text); v.ok()) {
        index_.upsert_item(conv_id, "episode", ep.id, transcript_text, *v.value);
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    summaries_[conv_id] = new_summary;
    auto it = working_.find(conv_id);
    if (it != working_.end() && it->second) it->second->restore(to_keep);
  }

  // 事实沉淀：对被压缩的这批 transcript 追加一次 consolidate()，
  // 把「事实 / 性格特征」写进长期记忆（facts.md / traits.md）。
  //
  // 为什么在这里而不是编排层：只有 compact 内部还持有 to_compact 这份原文，
  // 编排层调用 compact 后原文已经被 take/restore 掉，再读就是压缩后的剩余部分。
  // 为什么不放在持锁区：consolidate 会调一次 LLM（秒级）并回调
  // NotesStore/IndexStore（各自有锁），持 mu_ 调用会阻塞整个会话。
  // 上面那段作用域已经释放 mu_，这里调用不会递归加锁。
  //
  // 幂等：Consolidator::apply_json 对 facts 按 content 去重、episodes 按
  // summary 去重，重复调用不会重复写入。
  // 失败只打 warn：压缩本身已经成功，沉淀是增强项，不能反过来让主链路失败。
  if (!to_compact.empty()) {
    auto cr = consolidator.consolidate(conv_id, to_compact);
    if (!cr.ok()) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                            "consolidate_after_compact_failed")
          .field("detail", cr.error.detail);
    }
  }

  ConsolidateReport rep;
  rep.episodes_written = 1;
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "memory", "compacted")
      .field("compacted_msgs", static_cast<long long>(to_compact.size()))
      .field("kept_msgs", static_cast<long long>(to_keep.size()));
  return Result<ConsolidateReport>::ok(rep);
}

Result<std::vector<MemorySystem::SearchHit>> MemorySystem::search(const std::string& conv_id,
                                                                const std::string& query,
                                                                int limit) {
  std::vector<SearchHit> out;
  if (query.empty()) return Result<std::vector<SearchHit>>::ok(std::move(out));

  // 会话隔离（缺陷 2）：没有 conv_id 就无法保证不跨会话泄漏。
  // 空 conv_id 一律返回空结果 —— 宁可搜不到，也不能泄漏别人的记忆。
  if (conv_id.empty()) {
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                          "search_without_conv_id_rejected");
    return Result<std::vector<SearchHit>>::ok(std::move(out));
  }

  const std::size_t cap = limit > 0 ? static_cast<std::size_t>(limit) : std::numeric_limits<std::size_t>::max();

  // 向量路（若配了 embedding）
  if (embedding_ != nullptr) {
    if (auto v = embedding_->embed(query); v.ok()) {
      if (auto hits = index_.search(*v.value, static_cast<int>(cap), conv_id); hits.ok()) {
        for (const auto& h : *hits.value) {
          SearchHit sh;
          sh.kind = h.kind;
          sh.text = h.text;
          sh.score = h.score;
          out.push_back(std::move(sh));
        }
      }
    }
  }

  // 关键词路补足（向量不可用时也能用）
  //
  // score 语义：向量路是余弦相似度（0~1），关键词路是「精确命中 = 1.0」。
  // 两路**不融合**：结果按「向量路在前、关键词路补足」拼接，
  // 关键词路的 1.0 不参与跨路比较（否则关键词路会压过向量路）。
  // 关键词路内部已按相关度排好序（见 IndexStore::search_text）。
  if (out.size() < cap) {
    if (auto kw = index_.search_text(conv_id, query, static_cast<int>(cap)); kw.ok()) {
      for (const auto& h : *kw.value) {
        if (out.size() >= cap) break;
        const bool dup = std::any_of(out.begin(), out.end(),
                                     [&](const SearchHit& x) { return x.text == h.text; });
        if (dup) continue;
        SearchHit sh;
        sh.kind = h.kind;
        sh.text = h.text;
        sh.score = h.score;   // = 1.0（精确命中）
        out.push_back(std::move(sh));
      }
    }
  }

  return Result<std::vector<SearchHit>>::ok(std::move(out));
}

void MemorySystem::remember_meme(const std::string& conv_id, const std::string& term,
                                 const std::string& meaning) {
  if (conv_id.empty() || term.empty()) return;

  const std::string today_s = today();

  // ── trait：pattern = 梗名本身，evidence 累加 ──
  //
  // 为什么不写成"会用网络梗「xx」"这种长句：
  // traits.md 的 pattern 是**被检索的键**，模型召回时看到的就是它。
  // 写成梗名本身，语义更准（"这个梗我们记住了"），
  // 而且同一个梗再次出现时能靠 pattern 精确匹配上，累加 evidence。
  std::vector<Trait> traits;
  {
    auto loaded = notes_.load_traits(conv_id);
    if (loaded.ok()) traits = *loaded.value;
  }
  bool trait_hit = false;
  for (auto& t : traits) {
    if (t.pattern == term) {
      // 同一个梗重复出现 → evidence 累加（语义：见过几次）
      ++t.evidence;
      trait_hit = true;
      break;
    }
  }
  if (!trait_hit) {
    traits.push_back(Trait{term, 1});
  }
  {
    const auto e = notes_.save_traits(conv_id, traits);
    if (!e.ok()) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                            "meme_trait_save_failed")
          .field("detail", e.detail)
          .field("term", term);
    }
  }

  // ── fact：只记「对方在用这个梗」，**不记梗的释义** ──
  //
  // 为什么释义不进 facts：
  //   facts 是「关于用户」的画像，而梗的释义是**关于世界**的知识。
  //   把释义写进 facts 会污染用户画像；而且 facts 按会话隔离，
  //   同一个梗在另一个会话里就查不到了——而释义本来就该全局共享
  //   （它已经在 memes.tsv 里了）。
  //
  //   这里记的是用户侧信息：「对方在用 xx 这个梗」，
  //   这才是值得按会话记住的东西。
  //
  // id 用内容哈希：同一个梗重复记时 id 相同 → upsert 覆盖而不是新增，
  // 不会在 facts.md 里刷出一堆重复条目。
  const std::string fact_content = "对方在用网络梗「" + term + "」";

  auto content_hash = [](const std::string& s) {
    std::size_t h = 1469598103934665603ULL;
    for (const char c : s) {
      h ^= static_cast<unsigned char>(c);
      h *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
  };

  Fact fact;
  fact.id = "m" + content_hash(fact_content).substr(0, 12);
  fact.subject = "用户";
  fact.content = fact_content;
  fact.confidence = 0.7;
  fact.created = today_s;

  // 幂等：同内容已在 facts.md 里就不重复追加
  std::vector<Fact> existing;
  {
    auto loaded = notes_.load_facts(conv_id);
    if (loaded.ok()) existing = *loaded.value;
  }
  bool dup = false;
  for (const auto& f : existing) {
    if (f.content == fact.content) {
      dup = true;
      break;
    }
  }
  if (!dup) {
    const auto e = notes_.append_fact(conv_id, fact);
    if (!e.ok()) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                            "meme_fact_save_failed")
          .field("detail", e.detail)
          .field("term", term);
    }
  }

  // 索引：upsert 幂等，重复记同一个梗只是覆盖同一条。
  // trait 走 save_traits 只写 traits.md，**不进 SQLite 索引**，
  // 所以「学到了什么」要能被 search_memory 搜到，必须走这条 fact + upsert。
  if (embedding_ != nullptr) {
    auto vec = embedding_->embed(fact_content);
    if (vec.ok()) {
      const auto e = index_.upsert_item(conv_id, "fact", fact.id, fact_content, *vec.value);
      if (!e.ok()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                              "meme_index_failed")
            .field("detail", e.detail)
            .field("term", term);
      }
    }
  }

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "memory", "meme_remembered")
      .field("conversation", conv_id)
      .field("term", term)
      .field("evidence", static_cast<long long>(trait_hit ? 2 : 1));
}

void MemorySystem::set_summary(const std::string& conv_id, std::string summary) {
  std::lock_guard<std::mutex> lock(mu_);
  summaries_[conv_id] = std::move(summary);
}

int MemorySystem::round_count(const std::string& conv_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = working_.find(conv_id);
  if (it == working_.end()) return 0;
  return it->second->round_count();
}

Result<MemoryContext> MemorySystem::gather(const std::string& conv_id) {
  MemoryContext ctx;

  std::vector<Message> all;
  std::string summary;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = working_.find(conv_id);
    if (it != working_.end()) all = it->second->snapshot();
    auto sit = summaries_.find(conv_id);
    if (sit != summaries_.end()) summary = sit->second;
  }

  if (all.empty() && summary.empty()) return Result<MemoryContext>::ok(std::move(ctx));

  // 按 token 预算决定最近多少原文能全量放进去（从最新往回收）
  const auto budget = TokenBudget::make(cfg_.model_context_tokens, cfg_.raw_context_ratio,
                                        cfg_.summary_ratio);

  std::vector<Message> picked;
  std::size_t used = 0;
  for (auto it = all.rbegin(); it != all.rend(); ++it) {
    const std::size_t t = estimate_tokens(it->text) + 8;
    if (budget.raw_budget > 0 && used + t > budget.raw_budget && !picked.empty()) break;
    used += t;
    picked.push_back(*it);
  }
  std::reverse(picked.begin(), picked.end());

  ctx.working = std::move(picked);
  ctx.summary = std::move(summary);
  ctx.estimated_tokens = used + estimate_tokens(ctx.summary);
  return Result<MemoryContext>::ok(std::move(ctx));
}

Result<ConsolidateReport> MemorySystem::force_consolidate(const std::string& conv_id,
                                                          Consolidator& consolidator) {
  std::vector<Message> transcript;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = working_.find(conv_id);
    if (it == working_.end() || it->second->round_count() == 0) {
      ConsolidateReport r;
      r.skipped = true;
      r.reason = "no_working_memory";
      return Result<ConsolidateReport>::ok(r);
    }
    transcript = it->second->take_all();
  }

  auto result = consolidator.consolidate(conv_id, transcript);
  if (!result.ok()) {
    // 失败恢复，避免丢记忆；下轮立即重试
    std::lock_guard<std::mutex> lock(mu_);
    auto& wm = working_[conv_id];
    if (!wm) wm = std::make_unique<WorkingMemory>(0);
    wm->restore(transcript);
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                          "consolidate_failed_restored")
        .field("detail", result.error.detail);
    return result;
  }
  return result;
}

}  // namespace fish::memory

