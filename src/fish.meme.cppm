// src/fish.meme.cppm
// 网络热梗库：本地优先检索，外部搜索后入库。
//
// 设计：
//   - 独立存储区（data/memes/），与记忆分开：
//     热梗是「公共知识」，不随会话隔离；记忆是「私人上下文」
//   - 权威源是纯文本文件（人可读、可手改、可增删）
//   - 检索用关键词匹配（热梗短、数量少，不需要向量）
//   - 外部搜索到的新梗写回本地，下次直接用，省一次网络请求
//
// 为什么需要：
//   模型训练数据有截止日期，新梗它不知道。若每次都联网查，既慢又贵。
//   所以本地建库：查到一次就记住，之后零成本。
module;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

export module fish.meme;

import fish.core;
import fish.log;

export namespace fish::meme {

// 一条热梗
struct Entry {
  std::string term;        // 梗本身（"尊嘟假嘟"）
  std::string meaning;     // 含义解释
  std::string usage;       // 用法示例
  std::string origin;      // 来源（可选）
  std::string added_at;    // yyyy-mm-dd
  // 命中次数：用于排序，高频的优先返回
  int hits{0};
};

class MemeStore {
 public:
  struct Config {
    std::string dir{"data/memes"};
    // 单次检索最多返回几条
    int max_results{5};
  };

  explicit MemeStore(Config cfg);

  // 建目录 + 加载。幂等。
  fish::core::Error init();

  // 重新加载（外部手改文件后调用）
  fish::core::Error reload();

  // 本地检索：按词匹配 term / meaning / usage。
  // 命中会累加 hits 并异步落盘（不阻塞调用方）。
  std::vector<Entry> search(const std::string& query, int max_results = 0);

  // 是否已收录（避免重复入库）
  [[nodiscard]] bool has(const std::string& term) const;

  // 新增/更新一条。已存在则更新内容并保留 hits。
  fish::core::Error upsert(const Entry& e);

  // 批量导入（从外部搜索结果）
  int import_many(const std::vector<Entry>& entries);

  [[nodiscard]] std::size_t size() const;

  // 全部条目（快照）
  std::vector<Entry> all() const;

 private:
  fish::core::Error load();
  fish::core::Error save() const;
  [[nodiscard]] std::string file_path() const;

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

bool contains_ci(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return false;
  auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                        [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                        });
  return it != hay.end();
}

}  // namespace detail

MemeStore::MemeStore(Config cfg) : cfg_(std::move(cfg)) {}

std::string MemeStore::file_path() const { return cfg_.dir + "/memes.tsv"; }

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
      if (e.term.empty()) continue;
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

fish::core::Error MemeStore::save() const {
  const std::string path = file_path();
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      return fish::core::Error::make(fish::core::ErrorCode::kInternal, "memes_open");
    }
    out << "# 网络热梗库\n";
    out << "# 格式: term<TAB>meaning<TAB>usage<TAB>origin<TAB>added_at<TAB>hits\n";
    out << "# 直接手改这个文件即可增删，重启或 reload() 后生效。\n";
    out << "term\tmeaning\tusage\torigin\tadded_at\thits\n";
    for (const auto& e : items_) {
      out << e.term << '\t' << e.meaning << '\t' << e.usage << '\t' << e.origin << '\t'
          << e.added_at << '\t' << e.hits << '\n';
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
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
  return items_;
}

bool MemeStore::has(const std::string& term) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& e : items_) {
    if (e.term == term) return true;
  }
  return false;
}

std::vector<Entry> MemeStore::search(const std::string& query, int max_results) {
  std::vector<Entry> hits;
  if (query.empty()) return hits;
  const int limit = max_results > 0 ? max_results : cfg_.max_results;

  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& e : items_) {
      // 匹配词本身 / 含义 / 用法
      if (detail::contains_ci(e.term, query) ||
          detail::contains_ci(query, e.term) ||
          detail::contains_ci(e.meaning, query) ||
          detail::contains_ci(e.usage, query)) {
        ++e.hits;
        hits.push_back(e);
      }
    }
    // 高频优先
    std::sort(hits.begin(), hits.end(),
              [](const Entry& a, const Entry& b) { return a.hits > b.hits; });
    if (static_cast<int>(hits.size()) > limit) {
      hits.resize(static_cast<std::size_t>(limit));
    }
    // 命中次数变了，落盘（失败不影响检索）
    if (!hits.empty()) save();
  }
  return hits;
}

fish::core::Error MemeStore::upsert(const Entry& e) {
  if (e.term.empty()) {
    return fish::core::Error::make(fish::core::ErrorCode::kBadRequest, "meme_empty_term");
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    bool found = false;
    for (auto& existing : items_) {
      if (existing.term == e.term) {
        // 保留原 hits，更新内容
        const int old_hits = existing.hits;
        existing = e;
        existing.hits = old_hits;
        if (existing.added_at.empty()) existing.added_at = detail::today_str();
        found = true;
        break;
      }
    }
    if (!found) {
      Entry copy = e;
      if (copy.added_at.empty()) copy.added_at = detail::today_str();
      items_.push_back(std::move(copy));
    }
  }
  return save();
}

int MemeStore::import_many(const std::vector<Entry>& entries) {
  int added = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& e : entries) {
      if (e.term.empty()) continue;
      bool found = false;
      for (auto& existing : items_) {
        if (existing.term == e.term) {
          found = true;
          // 已有内容不覆盖，只补空字段
          if (existing.meaning.empty()) existing.meaning = e.meaning;
          if (existing.usage.empty()) existing.usage = e.usage;
          if (existing.origin.empty()) existing.origin = e.origin;
          break;
        }
      }
      if (!found) {
        Entry copy = e;
        if (copy.added_at.empty()) copy.added_at = detail::today_str();
        items_.push_back(std::move(copy));
        ++added;
      }
    }
  }
  if (added > 0) {
    save();
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "meme", "imported")
        .field("added", static_cast<long long>(added));
  }
  return added;
}

}  // namespace fish::meme
