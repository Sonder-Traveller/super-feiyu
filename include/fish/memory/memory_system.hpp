// include/fish/memory/memory_system.hpp
// 三层记忆的统一门面。编排层只依赖这个，不感知内部实现。
//
//   L1 感觉记忆：当前消息，不落地（由 orchestrator 持有）
//   L2 短时记忆：内存环形缓冲，最近 N 轮原文
//   L3 长时记忆：旁路 Markdown 文件（权威）+ SQLite 索引（可重建）
//                ├─ 3a 情景 episodic
//                ├─ 3b 语义 semantic (facts)
//                └─ 3c 程序 procedural (traits)
#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "fish/core/interfaces.hpp"
#include "fish/core/types.hpp"

namespace fish::memory {

// ── 数据结构 ────────────────────────────────────────────
struct Message {
  std::string role;  // "user" | "assistant"
  std::string text;
  core::TimePoint at{};
};

struct Fact {
  std::string id;
  std::string subject;     // 关于谁，默认 "用户"
  std::string content;
  double confidence{0.9};
  std::string created;     // yyyy-mm-dd
  std::string superseded_by;  // 被哪条取代，空表示有效
  double weight{1.0};      // 衰减后权重
};

struct Episode {
  std::string id;
  std::string summary;
  double importance{0.5};
  std::string created;
  double weight{1.0};
};

struct Trait {
  std::string pattern;     // 行为模式描述
  int evidence{1};         // 证据次数，>=3 才确立
};

// 每轮注入的上下文
struct MemoryContext {
  std::vector<Message> working;   // L2 原文
  std::vector<Fact> facts;        // L3b 召回
  std::vector<Episode> episodes;  // L3a 召回
  std::string traits_text;        // L3c 全文
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
  // 取走并清空（巩固后调用）
  std::vector<Message> take_all();
  void clear();
  void restore(const std::vector<Message>& msgs);

 private:
  int max_rounds_;
  mutable std::mutex mu_;
  std::deque<Message> msgs_;
};

// ── 旁路文件存储（权威源）────────────────────────────────
class NotesStore {
 public:
  explicit NotesStore(std::string notes_dir);

  core::Error ensure_dirs() const;

  // facts.md
  core::Result<std::vector<Fact>> load_facts() const;
  core::Error append_fact(const Fact& f) const;
  core::Error rewrite_facts(const std::vector<Fact>& facts) const;

  // episodes/yyyy-mm.md
  core::Error append_episode(const Episode& e) const;
  core::Result<std::vector<Episode>> load_episodes(int max_months = 24) const;

  // traits.md
  core::Result<std::vector<Trait>> load_traits() const;
  core::Error save_traits(const std::vector<Trait>& traits) const;

 private:
  std::vector<Fact> load_facts_locked() const;

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

  core::Error open();
  core::Error close();

  // 索引 facts / episodes，带向量
  core::Error upsert_item(const std::string& kind, const std::string& id,
                          const std::string& text, const std::vector<float>& vec);

  struct Hit {
    std::string kind;
    std::string id;
    std::string text;
    double score{0.0};
  };

  // 暴力余弦召回，kind 为空表示不限
  core::Result<std::vector<Hit>> search(const std::vector<float>& query, int top_k,
                                        const std::string& kind = "") const;

  // 关键词路：简单 LIKE 匹配
  core::Result<std::vector<Hit>> search_text(const std::string& keyword, int top_k) const;

  [[nodiscard]] std::size_t item_count() const;
  core::Error clear();

 private:
  std::string path_;
  void* db_{nullptr};  // sqlite3*
};

// ── 巩固器：L2 → L3 ─────────────────────────────────────
class Consolidator {
 public:
  Consolidator(core::IChatModel* chat, NotesStore* notes, IndexStore* index,
               core::IEmbeddingModel* embedding);

  // 用一次独立的 LLM 调用把对话提炼成结构化记忆
  core::Result<ConsolidateReport> consolidate(const std::vector<Message>& transcript);

  // 供测试：直接解析模型输出的 JSON
  static core::Result<ConsolidateReport> apply_json(const std::string& json_text,
                                                    const std::string& today,
                                                    NotesStore* notes, IndexStore* index,
                                                    core::IEmbeddingModel* embedding);

 private:
  core::IChatModel* chat_;
  NotesStore* notes_;
  IndexStore* index_;
  core::IEmbeddingModel* embedding_;
};

// ── 遗忘：权重衰减与淘汰 ─────────────────────────────────
class Forgetting {
 public:
  struct Params {
    double half_life_days{30.0};
    double min_weight{0.05};
    int max_facts{2000};
    int max_episodes{5000};
  };

  explicit Forgetting(Params p) : p_(p) {}

  // 计算 decayed weight
  [[nodiscard]] static double decay(double importance, double days_since_use,
                                    double half_life_days);

  // 标记应淘汰的条目
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
    double half_life_days{30.0};
    double min_weight{0.05};
    int max_facts{2000};
    std::string notes_dir{"data/memory/notes"};
    std::string db_path{"data/memory/index.db"};
  };

  MemorySystem(Config cfg, core::IEmbeddingModel* embedding);

  core::Error init();

  // 每轮：记录对话
  core::Error record(const std::string& conv_id, const Message& user, const Message& assistant);

  // 每轮：取上下文
  core::Result<MemoryContext> gather(const std::string& conv_id, const std::string& query);

  // 后台：判断是否该巩固，需要时执行
  core::Result<ConsolidateReport> maybe_consolidate(const std::string& conv_id,
                                                    Consolidator& consolidator);

  // 强制巩固（退出时调用）
  core::Result<ConsolidateReport> force_consolidate(const std::string& conv_id,
                                                    Consolidator& consolidator);

  [[nodiscard]] int round_count(const std::string& conv_id) const;

  // 供 Consolidator 装配使用（巩固器需要直接读写 L3 存储）
  [[nodiscard]] NotesStore& notes() { return notes_; }
  [[nodiscard]] IndexStore& index() { return index_; }

 private:
  std::string today() const;

  Config cfg_;
  core::IEmbeddingModel* embedding_;
  NotesStore notes_;
  IndexStore index_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<WorkingMemory>> working_;
  std::unordered_map<std::string, int> rounds_since_consolidate_;
};

}  // namespace fish::memory
