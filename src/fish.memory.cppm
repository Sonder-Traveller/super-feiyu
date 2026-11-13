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
  std::vector<Message> working;
  std::vector<Fact> facts;
  std::vector<Episode> episodes;
  std::string traits_text;
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
  fish::core::Result<std::vector<Fact>> load_facts() const;
  fish::core::Error append_fact(const Fact& f) const;
  // 全量改写（按 subject 分组），用于淘汰后重写权威文件
  fish::core::Error rewrite_facts(const std::vector<Fact>& facts) const;
  fish::core::Error append_episode(const Episode& e) const;
  // 全量改写情景记忆（按月份分文件），用于容量淘汰后重写
  fish::core::Error rewrite_episodes(const std::vector<Episode>& eps) const;
  fish::core::Result<std::vector<Episode>> load_episodes(int max_months = 24) const;
  fish::core::Result<std::vector<Trait>> load_traits() const;
  fish::core::Error save_traits(const std::vector<Trait>& traits) const;

 private:
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
  fish::core::Result<ConsolidateReport> consolidate(const std::vector<Message>& transcript);
  static fish::core::Result<ConsolidateReport> apply_json(
      const std::string& json_text, const std::string& today, NotesStore* notes,
      IndexStore* index, fish::core::IEmbeddingModel* embedding);

 private:
  fish::core::IChatModel* chat_;
  NotesStore* notes_;
  IndexStore* index_;
  fish::core::IEmbeddingModel* embedding_;
};

// ── 遗忘 ────────────────────────────────────────────────
class Forgetting {
 public:
  struct Params {
    double half_life_days{30.0};
    double min_weight{0.05};
  };
  explicit Forgetting(Params p) : p_(p) {}
  [[nodiscard]] static double decay(double importance, double days_since_use,
                                    double half_life_days);
  [[nodiscard]] std::vector<std::string> select_evictions(
      const std::vector<Fact>& facts, double days_since_use) const;

 private:
  Params p_;
};

// ── 统一门面 ────────────────────────────────────────────
class MemorySystem {
 public:
  struct Config {
    int working_rounds{20};
    int consolidate_every{20};
    int recall_top_k{5};
    int episode_top_k{3};
    // 容量上限：超过就按权重淘汰（不是按时间）
    int max_facts{2000};
    int max_episodes{5000};
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
  [[nodiscard]] NotesStore& notes() { return notes_; }

  // 周期维护：按衰减权重淘汰旧记忆。由 runtime 定时调用（建议每天一次）。
  struct MaintainReport {
    int facts_before{0};
    int facts_after{0};
    int episodes_before{0};
    int episodes_after{0};
    int evicted_facts{0};
    int evicted_episodes{0};
    bool skipped{false};
  };
  // 按容量淘汰：总量超过配额时，保留权重最高的 max_facts / max_episodes 条。
  // 权重 = confidence × 时间衰减（越新越重要），所以不是单纯删最旧，
  // 而是"又旧又不重要"的先走。
  core::Result<MaintainReport> maintain(int max_facts, int max_episodes);
  [[nodiscard]] IndexStore& index() { return index_; }

 private:
  std::string today() const;

  Config cfg_;
  fish::core::IEmbeddingModel* embedding_;
  NotesStore notes_;
  IndexStore index_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<WorkingMemory>> working_;
  std::unordered_map<std::string, int> rounds_since_consolidate_;
};

}  // namespace fish::memory

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

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
    : max_rounds_(max_rounds <= 0 ? 1 : max_rounds) {}

void WorkingMemory::push(const Message& user, const Message& assistant) {
  std::lock_guard<std::mutex> lock(mu_);
  msgs_.push_back(user);
  msgs_.push_back(assistant);
  const std::size_t limit = static_cast<std::size_t>(max_rounds_) * 2;
  while (msgs_.size() > limit) msgs_.pop_front();
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
  const std::size_t limit = static_cast<std::size_t>(max_rounds_) * 2;
  while (msgs_.size() > limit) msgs_.pop_front();
}

// ── NotesStore ──────────────────────────────────────────
NotesStore::NotesStore(std::string notes_dir) : dir_(std::move(notes_dir)) {}

Error NotesStore::ensure_dirs() const {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  if (ec) return Error::make(ErrorCode::kInternal, "mkdir:" + ec.message());
  fs::create_directories(dir_ + "/episodes", ec);
  if (ec) return Error::make(ErrorCode::kInternal, "mkdir_episodes:" + ec.message());
  return Error::none();
}

Result<std::vector<Fact>> NotesStore::load_facts() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream in(dir_ + "/facts.md");
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

Error NotesStore::append_fact(const Fact& f) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  fs::create_directories(dir_, ec);

  // 读现有内容（已持锁，内联读取避免重入）
  std::vector<Fact> all;
  {
    std::ifstream in(dir_ + "/facts.md");
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

  const std::string path = dir_ + "/facts.md";
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

Error NotesStore::rewrite_facts(const std::vector<Fact>& facts) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  fs::create_directories(dir_, ec);

  const std::string path = dir_ + "/facts.md";
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

Error NotesStore::append_episode(const Episode& e) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::string month = e.created.size() >= 7 ? e.created.substr(0, 7) : "unknown";

  std::error_code ec;
  fs::create_directories(dir_ + "/episodes", ec);
  const std::string path = dir_ + "/episodes/" + month + ".md";
  const bool fresh = !fs::exists(path);

  std::ofstream out(path, std::ios::app);
  if (!out) return Error::make(ErrorCode::kInternal, "open_episode");
  if (fresh) out << "# " << month << " 情景记忆\n\n";
  out << "- [id:" << e.id << "] " << e.summary << " (重要度 " << e.importance << ", "
      << e.created << ")\n";
  return Error::none();
}

Error NotesStore::rewrite_episodes(const std::vector<Episode>& eps) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  const std::string ep_dir = dir_ + "/episodes";
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

Result<std::vector<Episode>> NotesStore::load_episodes(int max_months) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Episode> out;
  std::error_code ec;
  const fs::path ep_dir = dir_ + "/episodes";
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

Result<std::vector<Trait>> NotesStore::load_traits() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream in(dir_ + "/traits.md");
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

Error NotesStore::save_traits(const std::vector<Trait>& traits) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  fs::create_directories(dir_, ec);
  const std::string tmp = dir_ + "/traits.md.tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return Error::make(ErrorCode::kInternal, "open_traits");
    out << "# 行为模式与偏好\n\n<!-- 证据 >= 3 才确立 -->\n\n";
    for (const auto& t : traits) {
      out << "- " << t.pattern << " (证据 " << t.evidence << ")\n";
    }
  }
  fs::rename(tmp, dir_ + "/traits.md", ec);
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

Result<ConsolidateReport> Consolidator::consolidate(const std::vector<Message>& transcript) {
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

  return apply_json(json_extract(resp.value->text), date, notes_, index_, embedding_);
}

Result<ConsolidateReport> Consolidator::apply_json(const std::string& json_text,
                                                   const std::string& today,
                                                   NotesStore* notes, IndexStore* index,
                                                   fish::core::IEmbeddingModel* embedding) {
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
    auto ee = notes->load_episodes();
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
    if (notes != nullptr) notes->append_episode(ep);
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
    auto ef = notes->load_facts();
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
    if (notes != nullptr) notes->append_fact(fact);
    existing_facts.push_back(fact);
    if (index != nullptr && embedding != nullptr) {
      auto vec = embedding->embed(fact.content);
      if (vec.ok()) index->upsert_item("fact", fact.id, fact.content, *vec.value);
    }
    ++report.facts_written;
  }

  auto existing = notes != nullptr ? notes->load_traits()
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
  if (notes != nullptr && !traits.empty()) notes->save_traits(traits);

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "memory", "consolidated")
      .field("episodes", report.episodes_written)
      .field("facts", report.facts_written)
      .field("traits", report.traits_updated);

  return Result<ConsolidateReport>::ok(report);
}

// ── Forgetting ──────────────────────────────────────────
double Forgetting::decay(double importance, double days_since_use, double half_life_days) {
  if (half_life_days <= 0.0) return importance;
  return importance * std::pow(0.5, days_since_use / half_life_days);
}

std::vector<std::string> Forgetting::select_evictions(const std::vector<Fact>& facts,
                                                      double days_since_use) const {
  std::vector<std::pair<double, std::string>> scored;
  for (const auto& f : facts) {
    if (!f.superseded_by.empty()) continue;
    const double w = decay(f.confidence, days_since_use, p_.half_life_days);
    if (w < p_.min_weight) scored.emplace_back(w, f.id);
  }
  std::sort(scored.begin(), scored.end());
  std::vector<std::string> ids;
  for (const auto& pr : scored) ids.push_back(pr.second);
  return ids;
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

  // 索引为空但旁路文件有内容 → 重建
  if (index_.item_count() == 0 && embedding_ != nullptr) {
    std::size_t rebuilt = 0;
    auto facts = notes_.load_facts();
    if (facts.ok()) {
      for (const auto& f : *facts.value) {
        auto vec = embedding_->embed(f.content);
        if (vec.ok()) {
          index_.upsert_item("fact", f.id, f.content, *vec.value);
          ++rebuilt;
        }
      }
    }
    auto episodes = notes_.load_episodes();
    if (episodes.ok()) {
      for (const auto& ep : *episodes.value) {
        auto vec = embedding_->embed(ep.summary);
        if (vec.ok()) {
          index_.upsert_item("episode", ep.id, ep.summary, *vec.value);
          ++rebuilt;
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

Result<MemorySystem::MaintainReport> MemorySystem::maintain(int max_facts,
                                                         int max_episodes) {
  MaintainReport rep;

  // 权重 = confidence × 0.5^(age_days / half_life)
  // 为什么不是单纯按时间删：一条三个月前但置信度很高的事实，
  // 比一条昨天随口说的更该留下。时间衰减只作为排序因子，不单独构成删除条件。
  constexpr double kHalfLifeDays = 30.0;
  const std::string now_day = today();

  auto day_diff = [](const std::string& from, const std::string& to) -> double {
    std::tm a{}, b{};
    if (std::sscanf(from.c_str(), "%d-%d-%d", &a.tm_year, &a.tm_mon, &a.tm_mday) != 3) {
      return 0.0;
    }
    if (std::sscanf(to.c_str(), "%d-%d-%d", &b.tm_year, &b.tm_mon, &b.tm_mday) != 3) {
      return 0.0;
    }
    a.tm_year -= 1900;
    a.tm_mon -= 1;
    b.tm_year -= 1900;
    b.tm_mon -= 1;
    const std::time_t ta = std::mktime(&a);
    const std::time_t tb = std::mktime(&b);
    if (ta < 0 || tb < 0) return 0.0;
    const double d = std::difftime(tb, ta) / 86400.0;
    return d < 0.0 ? 0.0 : d;  // 未来日期按 0 处理
  };

  // ── 事实 ──
  auto facts_r = notes_.load_facts();
  if (!facts_r.ok()) {
    return Result<MaintainReport>::fail(facts_r.error.code, facts_r.error.detail);
  }
  std::vector<Fact> facts = std::move(*facts_r.value);
  rep.facts_before = static_cast<int>(facts.size());

  if (max_facts > 0 && static_cast<int>(facts.size()) > max_facts) {
    // 已被取代的条目单独处理：不参与权重竞争，直接随其替代者去留
    std::vector<std::pair<double, std::size_t>> ranked;
    ranked.reserve(facts.size());
    for (std::size_t i = 0; i < facts.size(); ++i) {
      const double age = day_diff(facts[i].created, now_day);
      const double w = facts[i].confidence * std::pow(0.5, age / kHalfLifeDays);
      ranked.emplace_back(w, i);
    }
    // 权重高的在前
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<bool> keep(facts.size(), false);
    for (int i = 0; i < max_facts && i < static_cast<int>(ranked.size()); ++i) {
      keep[ranked[static_cast<std::size_t>(i)].second] = true;
    }

    std::vector<Fact> kept;
    kept.reserve(static_cast<std::size_t>(max_facts));
    for (std::size_t i = 0; i < facts.size(); ++i) {
      if (keep[i]) {
        kept.push_back(std::move(facts[i]));
      } else {
        index_.remove_item("fact", facts[i].id);
        ++rep.evicted_facts;
      }
    }
    if (!kept.empty() || rep.evicted_facts > 0) {
      auto e = notes_.rewrite_facts(kept);
      if (!e.ok()) {
        return Result<MaintainReport>::fail(e.code, e.detail);
      }
    }
    facts = std::move(kept);
  }
  rep.facts_after = static_cast<int>(facts.size());

  // ── 情景 ──
  auto eps_r = notes_.load_episodes(240);
  if (eps_r.ok()) {
    std::vector<Episode> eps = std::move(*eps_r.value);
    rep.episodes_before = static_cast<int>(eps.size());

    if (max_episodes > 0 && static_cast<int>(eps.size()) > max_episodes) {
      std::vector<std::pair<double, std::size_t>> ranked;
      ranked.reserve(eps.size());
      for (std::size_t i = 0; i < eps.size(); ++i) {
        const double age = day_diff(eps[i].created, now_day);
        const double w = eps[i].importance * std::pow(0.5, age / kHalfLifeDays);
        ranked.emplace_back(w, i);
      }
      std::sort(ranked.begin(), ranked.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });

      std::vector<bool> keep(eps.size(), false);
      for (int i = 0; i < max_episodes && i < static_cast<int>(ranked.size()); ++i) {
        keep[ranked[static_cast<std::size_t>(i)].second] = true;
      }

      std::vector<Episode> kept;
      kept.reserve(static_cast<std::size_t>(max_episodes));
      for (std::size_t i = 0; i < eps.size(); ++i) {
        if (keep[i]) {
          kept.push_back(std::move(eps[i]));
        } else {
          index_.remove_item("episode", eps[i].id);
          ++rep.evicted_episodes;
        }
      }
      if (rep.evicted_episodes > 0) {
        notes_.rewrite_episodes(kept);
      }
      eps = std::move(kept);
    }
    rep.episodes_after = static_cast<int>(eps.size());
  }

  if (rep.evicted_facts > 0 || rep.evicted_episodes > 0) {
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "memory", "capacity_evicted")
        .field("facts_before", rep.facts_before)
        .field("facts_after", rep.facts_after)
        .field("episodes_before", rep.episodes_before)
        .field("episodes_after", rep.episodes_after)
        .field("evicted_facts", rep.evicted_facts)
        .field("evicted_episodes", rep.evicted_episodes);
  } else {
    rep.skipped = true;
  }

  return Result<MaintainReport>::ok(rep);
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
  if (!wm) wm = std::make_unique<WorkingMemory>(cfg_.working_rounds);
  wm->push(user, assistant);
  ++rounds_since_consolidate_[conv_id];
  return Error::none();
}

int MemorySystem::round_count(const std::string& conv_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = working_.find(conv_id);
  if (it == working_.end()) return 0;
  return it->second->round_count();
}

Result<MemoryContext> MemorySystem::gather(const std::string& conv_id,
                                           const std::string& query) {
  MemoryContext ctx;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = working_.find(conv_id);
    if (it != working_.end()) ctx.working = it->second->snapshot();
  }

  // L3c 程序记忆：证据 >= 3 才注入
  auto traits = notes_.load_traits();
  if (traits.ok()) {
    std::string text;
    for (const auto& t : *traits.value) {
      if (t.evidence < 3) continue;
      text += "- " + t.pattern + "\n";
    }
    ctx.traits_text = text;
  }

  if (embedding_ == nullptr || query.empty()) {
    return Result<MemoryContext>::ok(std::move(ctx));
  }

  const std::string kw_query = query.substr(0, std::min<std::size_t>(query.size(), 20));

  auto query_vec = embedding_->embed(query);
  if (!query_vec.ok()) {
    // 向量失败 → 退化关键词召回，不阻断
    auto kw = index_.search_text(kw_query, 8);
    if (kw.ok()) {
      for (const auto& h : *kw.value) {
        if (h.kind == "fact" && static_cast<int>(ctx.facts.size()) < cfg_.recall_top_k) {
          Fact f;
          f.id = h.id;
          f.content = h.text;
          ctx.facts.push_back(std::move(f));
        }
      }
    }
    return Result<MemoryContext>::ok(std::move(ctx));
  }

  // 三路：向量 + 关键词，RRF 融合
  auto vec_hits = index_.search(*query_vec.value, 20);
  auto kw_hits = index_.search_text(kw_query, 20);

  constexpr double kRrf = 60.0;
  std::unordered_map<std::string, std::pair<double, IndexStore::Hit>> fused;
  auto accumulate = [&](const std::vector<IndexStore::Hit>& hits) {
    for (std::size_t r = 0; r < hits.size(); ++r) {
      const std::string key = hits[r].kind + "/" + hits[r].id;
      fused[key].first += 1.0 / (kRrf + static_cast<double>(r + 1));
      fused[key].second = hits[r];
    }
  };
  if (vec_hits.ok()) accumulate(*vec_hits.value);
  if (kw_hits.ok()) accumulate(*kw_hits.value);

  std::vector<std::pair<double, IndexStore::Hit>> ranked;
  ranked.reserve(fused.size());
  for (auto& pr : fused) ranked.emplace_back(pr.second.first, pr.second.second);
  std::sort(ranked.begin(), ranked.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  for (const auto& pr : ranked) {
    const auto& hit = pr.second;
    if (hit.kind == "fact" && static_cast<int>(ctx.facts.size()) < cfg_.recall_top_k) {
      Fact f;
      f.id = hit.id;
      f.content = hit.text;
      ctx.facts.push_back(std::move(f));
    } else if (hit.kind == "episode" &&
               static_cast<int>(ctx.episodes.size()) < cfg_.episode_top_k) {
      Episode e;
      e.id = hit.id;
      e.summary = hit.text;
      ctx.episodes.push_back(std::move(e));
    }
    if (static_cast<int>(ctx.facts.size()) >= cfg_.recall_top_k &&
        static_cast<int>(ctx.episodes.size()) >= cfg_.episode_top_k) {
      break;
    }
  }

  return Result<MemoryContext>::ok(std::move(ctx));
}

Result<ConsolidateReport> MemorySystem::maybe_consolidate(const std::string& conv_id,
                                                          Consolidator& consolidator) {
  bool should = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = rounds_since_consolidate_.find(conv_id);
    should = it != rounds_since_consolidate_.end() && it->second >= cfg_.consolidate_every;
  }
  if (!should) {
    ConsolidateReport r;
    r.skipped = true;
    r.reason = "not_due";
    return Result<ConsolidateReport>::ok(r);
  }
  return force_consolidate(conv_id, consolidator);
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
    rounds_since_consolidate_[conv_id] = 0;
  }

  auto result = consolidator.consolidate(transcript);
  if (!result.ok()) {
    // 失败恢复，避免丢记忆；下轮立即重试
    std::lock_guard<std::mutex> lock(mu_);
    auto& wm = working_[conv_id];
    if (!wm) wm = std::make_unique<WorkingMemory>(cfg_.working_rounds);
    wm->restore(transcript);
    rounds_since_consolidate_[conv_id] = cfg_.consolidate_every;
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "memory",
                          "consolidate_failed_restored")
        .field("detail", result.error.detail);
    return result;
  }
  return result;
}

}  // namespace fish::memory

