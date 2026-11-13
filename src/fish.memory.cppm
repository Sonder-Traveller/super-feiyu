// src/fish.memory.cppm
// 三层记忆：L2 短时（内存）+ L3 长时（旁路 Markdown 权威 + SQLite 可重建索引）
module;
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
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
  std::size_t total{0};
  std::size_t raw_budget{0};      // 原文可用
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
  explicit WorkingMemory(int max_rounds = 20);
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

  fish::core::Result<std::vector<Fact>> load_facts(const std::string& conv_id) const;
  fish::core::Error append_fact(const std::string& conv_id, const Fact& f) const;
  // 全量改写（按 subject 分组），用于淘汰后重写权威文件
  fish::core::Error rewrite_facts(const std::string& conv_id,
                                  const std::vector<Fact>& facts) const;
  fish::core::Error append_episode(const std::string& conv_id, const Episode& e) const;
  // 全量改写情景记忆（按月份分文件），用于容量淘汰后重写
  fish::core::Error rewrite_episodes(const std::string& conv_id,
                                     const std::vector<Episode>& eps) const;
  fish::core::Result<std::vector<Episode>> load_episodes(const std::string& conv_id,
                                                         int max_months = 24) const;
  fish::core::Result<std::vector<Trait>> load_traits(const std::string& conv_id) const;
  fish::core::Error save_traits(const std::string& conv_id,
                                const std::vector<Trait>& traits) const;

 private:
  fish::core::Error ensure_session_dir(const std::string& conv_id) const;

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
  fish::core::Error upsert_item(const std::string& kind, const std::string& id,
                                const std::string& text, const std::vector<float>& vec);
  fish::core::Error remove_item(const std::string& kind, const std::string& id);

  struct Hit {
    std::string kind;
    std::string id;
    std::string text;
    double score{0.0};
  };

  fish::core::Result<std::vector<Hit>> search(const std::vector<float>& query, int top_k,
                                              const std::string& kind = "") const;
  fish::core::Result<std::vector<Hit>> search_text(const std::string& keyword,
                                                   int top_k) const;
  [[nodiscard]] std::size_t item_count() const;

 private:
  std::string path_;
  void* db_{nullptr};
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
    double raw_context_ratio{0.40};     // 原文可占比例
    double summary_ratio{0.25};         // 摘要可占余下部分的比例
    // 容量上限：超过就按权重淘汰（不是按时间）

    std::string notes_dir{"data/memory/notes"};
    std::string db_path{"data/memory/index.db"};
  };

  MemorySystem(Config cfg, fish::core::IEmbeddingModel* embedding);
  fish::core::Error init();
  fish::core::Error record(const std::string& conv_id, const Message& user,
                           const Message& assistant);
  fish::core::Result<MemoryContext> gather(const std::string& conv_id,
                                           const std::string& query);
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
  struct SearchHit {
    std::string kind;     // "fact" / "episode"
    std::string text;
    double score{0.0};
  };
  core::Result<std::vector<SearchHit>> search(const std::string& query, int limit);

  [[nodiscard]] NotesStore& notes() { return notes_; }
  [[nodiscard]] IndexStore& index() { return index_; }

 private:
  std::string today() const;

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

  double rr = raw_ratio;
  if (!(rr > 0.0)) rr = 0.40;
  if (rr > 0.95) rr = 0.95;
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

std::string render_transcript(const std::vector<Message>& msgs) {
  std::ostringstream oss;
  for (const auto& m : msgs) {
    oss << (m.role == "user" ? "对方: " : "我: ") << m.text << "\n";
  }
  return oss.str();
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
  return Error::none();
}

std::string NotesStore::session_dir(const std::string& conv_id) const {
  // 会话 id 直接做目录名不安全（可能含 / : 等），只保留 [A-Za-z0-9-_]。
  std::string safe;
  safe.reserve(conv_id.size());
  for (const char c : conv_id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    safe += ok ? c : '_';
  }
  if (safe.empty()) safe = "default";
  // 前缀区分群聊/私聊，避免 openid 恰好同名的极端情况
  const std::string kind = (conv_id.rfind("g:", 0) == 0) ? "group" : "user";
  return dir_ + "/" + kind + "/" + safe;
}

Error NotesStore::ensure_session_dir(const std::string& conv_id) const {
  std::error_code ec;
  fs::create_directories(session_dir(conv_id) + "/episodes", ec);
  if (ec) return Error::make(ErrorCode::kInternal, "mkdir_session:" + ec.message());
  return Error::none();
}

Result<std::vector<Fact>> NotesStore::load_facts(const std::string& conv_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream in(session_dir(conv_id) + "/facts.md");
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

  // 读现有内容（已持锁，内联读取避免重入）
  std::vector<Fact> all;
  {
    std::ifstream in(sdir + "/facts.md");
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
  out << "- [id:" << e.id << "] " << e.summary << " (重要度 " << e.importance << ", "
      << e.created << ")\n";
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
        out << "- [id:" << e.id << "] " << e.summary << " (重要度 " << e.importance
            << ", " << e.created << ")\n";
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
  const fs::path ep_dir = session_dir(conv_id) + "/episodes";
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
  for (const auto& f : files) {
    std::ifstream in(f);
    std::string line;
    while (std::getline(in, line)) {
      std::smatch m;
      if (!std::regex_match(line, m, re)) continue;
      Episode e;
      e.id = m[1];
      e.summary = m[2];
      e.importance = m[3].matched ? std::stod(m[3]) : 0.5;
      e.created = m[4].matched ? std::string(m[4]) : "";
      out.push_back(std::move(e));
    }
  }
  return Result<std::vector<Episode>>::ok(std::move(out));
}

Result<std::vector<Trait>> NotesStore::load_traits(const std::string& conv_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream in(session_dir(conv_id) + "/traits.md");
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

  const char* ddl =
      "CREATE TABLE IF NOT EXISTS items (kind TEXT NOT NULL, id TEXT NOT NULL,"
      " text TEXT NOT NULL, vec BLOB, dim INTEGER NOT NULL DEFAULT 0,"
      " PRIMARY KEY (kind, id));"
      "CREATE INDEX IF NOT EXISTS idx_kind ON items(kind);";
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

Error IndexStore::upsert_item(const std::string& kind, const std::string& id,
                              const std::string& text, const std::vector<float>& vec) {
  if (db_ == nullptr) return Error::make(ErrorCode::kInternal, "db_closed");

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO items(kind,id,text,vec,dim) VALUES(?,?,?,?,?) "
      "ON CONFLICT(kind,id) DO UPDATE SET text=excluded.text,"
      "vec=excluded.vec,dim=excluded.dim;";
  if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error::make(ErrorCode::kInternal, "prepare");
  }

  const std::string blob = serialize_vec(vec);
  sqlite3_bind_text(stmt, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, text.c_str(), -1, SQLITE_TRANSIENT);
  if (blob.empty()) {
    sqlite3_bind_null(stmt, 4);
  } else {
    sqlite3_bind_blob(stmt, 4, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
  }
  sqlite3_bind_int(stmt, 5, static_cast<int>(vec.size()));

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return Error::make(ErrorCode::kInternal, "upsert");
  return Error::none();
}

Result<std::vector<IndexStore::Hit>> IndexStore::search(const std::vector<float>& query,
                                                        int top_k,
                                                        const std::string& kind) const {
  if (db_ == nullptr) return Result<std::vector<Hit>>::fail(ErrorCode::kInternal, "db_closed");
  if (query.empty() || top_k <= 0) return Result<std::vector<Hit>>::ok({});

  const std::string sql =
      kind.empty() ? "SELECT kind,id,text,vec FROM items WHERE vec IS NOT NULL;"
                   : "SELECT kind,id,text,vec FROM items WHERE vec IS NOT NULL AND kind=?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql.c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return Result<std::vector<Hit>>::fail(ErrorCode::kInternal, "prepare");
  }
  if (!kind.empty()) sqlite3_bind_text(stmt, 1, kind.c_str(), -1, SQLITE_TRANSIENT);

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

Result<std::vector<IndexStore::Hit>> IndexStore::search_text(const std::string& keyword,
                                                             int top_k) const {
  if (db_ == nullptr) return Result<std::vector<Hit>>::fail(ErrorCode::kInternal, "db_closed");
  if (keyword.empty() || top_k <= 0) return Result<std::vector<Hit>>::ok({});

  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT kind,id,text FROM items WHERE text LIKE ? LIMIT ?;";
  if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Result<std::vector<Hit>>::fail(ErrorCode::kInternal, "prepare");
  }
  const std::string pattern = "%" + keyword + "%";
  sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, top_k);

  std::vector<Hit> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    Hit h;
    const auto* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const auto* i = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    h.kind = k != nullptr ? k : "";
    h.id = i != nullptr ? i : "";
    h.text = t != nullptr ? t : "";
    h.score = 1.0;
    out.push_back(std::move(h));
  }
  sqlite3_finalize(stmt);
  return Result<std::vector<Hit>>::ok(std::move(out));
}

Error IndexStore::remove_item(const std::string& kind, const std::string& id) {
  if (db_ == nullptr) return Error::make(ErrorCode::kInternal, "db_closed");
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "DELETE FROM items WHERE kind=? AND id=?;";
  if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error::make(ErrorCode::kInternal, "prepare_remove");
  }
  sqlite3_bind_text(stmt, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return Error::make(ErrorCode::kInternal, "remove_failed");
  return Error::none();
}

std::size_t IndexStore::item_count() const {
  if (db_ == nullptr) return 0;
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
      if (vec.ok()) index->upsert_item("episode", ep.id, ep.summary, *vec.value);
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
      if (vec.ok()) index->upsert_item("fact", fact.id, fact.content, *vec.value);
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

  // 索引为空但旁路文件有内容 → 重建。
  // 记忆按会话分目录（notes/<kind>/<safe_id>/），所以遍历所有会话，
  // 把每个会话的 facts/episodes 都重新灌进索引。
  if (index_.item_count() == 0 && embedding_ != nullptr) {
    std::size_t rebuilt = 0;
    std::error_code dir_ec;
    for (const auto& kind : {"group", "user"}) {
      const fs::path base = fs::path(cfg_.notes_dir) / kind;
      if (!fs::exists(base, dir_ec)) continue;
      for (const auto& entry : fs::directory_iterator(base, dir_ec)) {
        if (!entry.is_directory()) continue;
        // 目录名是脱敏后的会话 id，重建时用它反推 conv_id 去读文件
        const std::string safe_id = entry.path().filename().string();
        const std::string conv_id =
            std::string(kind) == "group" ? ("g:" + safe_id) : safe_id;

        auto facts = notes_.load_facts(conv_id);
        if (facts.ok()) {
          for (const auto& f : *facts.value) {
            auto vec = embedding_->embed(f.content);
            if (vec.ok()) {
              index_.upsert_item("fact", f.id, f.content, *vec.value);
              ++rebuilt;
            }
          }
        }
        auto episodes = notes_.load_episodes(conv_id);
        if (episodes.ok()) {
          for (const auto& ep : *episodes.value) {
            auto vec = embedding_->embed(ep.summary);
            if (vec.ok()) {
              index_.upsert_item("episode", ep.id, ep.summary, *vec.value);
              ++rebuilt;
            }
          }
        }
      }
    }
    if (rebuilt > 0) {
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "memory", "index_rebuilt")
          .field("items", static_cast<long long>(rebuilt));
    }
  }

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "memory", "init_ok")
      .field("db", cfg_.db_path)
      .field("notes", cfg_.notes_dir)
      .field("items", static_cast<long long>(index_.item_count()));
  return Error::none();
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
  wm->push(user, assistant);
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
        index_.upsert_item("episode", ep.id, transcript_text, *v.value);
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    summaries_[conv_id] = new_summary;
    auto it = working_.find(conv_id);
    if (it != working_.end() && it->second) it->second->restore(to_keep);
  }

  ConsolidateReport rep;
  rep.episodes_written = 1;
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "memory", "compacted")
      .field("compacted_msgs", static_cast<long long>(to_compact.size()))
      .field("kept_msgs", static_cast<long long>(to_keep.size()));
  return Result<ConsolidateReport>::ok(rep);
}

Result<std::vector<MemorySystem::SearchHit>> MemorySystem::search(const std::string& query,
                                                                 int limit) {
  std::vector<SearchHit> out;
  if (query.empty()) return Result<std::vector<SearchHit>>::ok(std::move(out));

  const std::size_t cap = limit > 0 ? static_cast<std::size_t>(limit) : std::numeric_limits<std::size_t>::max();

  // 向量路（若配了 embedding）
  if (embedding_ != nullptr) {
    if (auto v = embedding_->embed(query); v.ok()) {
      if (auto hits = index_.search(*v.value, static_cast<int>(cap)); hits.ok()) {
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
  if (out.size() < cap) {
    if (auto kw = index_.search_text(query, static_cast<int>(cap)); kw.ok()) {
      for (const auto& h : *kw.value) {
        if (out.size() >= cap) break;
        const bool dup = std::any_of(out.begin(), out.end(),
                                     [&](const SearchHit& x) { return x.text == h.text; });
        if (dup) continue;
        SearchHit sh;
        sh.kind = h.kind;
        sh.text = h.text;
        sh.score = 0.0;
        out.push_back(std::move(sh));
      }
    }
  }

  return Result<std::vector<SearchHit>>::ok(std::move(out));
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

Result<MemoryContext> MemorySystem::gather(const std::string& conv_id,
                                           const std::string& /*query*/) {
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

