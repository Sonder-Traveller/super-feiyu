// src/fish.image.cppm
// 形象库：管理「大肥鱼自己长什么样」的图片资产。
//
// 设计要点（与记忆系统同构）：
//   - 权威源是旁路文件（manifest.md + 图片文件），人可读、可手改、可增删
//   - 索引只是缓存，可从文件重建
//   - 不做「按时间淘汰」：图片是资产，不是对话，不设时效
//
// 与 IImageModel 的分工：
//   - 形象库回答「我有哪些现成的图」
//   - IImageModel 回答「我现在要生成一张新图」
//   模型先查库，命中就发库存图（零成本、形象稳定）；
//   没命中且允许生图时，才调 IImageModel。
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

export module fish.image;

import fish.core;
import fish.log;

export namespace fish::image {

// 一张形象图
struct Portrait {
  std::string path;        // 本地路径或 URL
  std::string caption;     // 这张图是什么（"在吃泡面"、"生气的样子"）
  std::vector<std::string> tags;   // 检索用：["吃", "泡面", "夜宵"]
  std::string source;      // "manual" | "generated" | "uploaded"
  std::string created_at;  // yyyy-mm-dd
};

// 选择策略
enum class PickStrategy : std::uint8_t {
  kRandom,     // 随机（默认，像人随手拍）
  kTagMatch,   // 按标签匹配当前话题
};

class PortraitStore {
 public:
  explicit PortraitStore(std::string dir);

  // 建目录、读 manifest。幂等。
  fish::core::Error init();

  // 重新从磁盘加载 manifest（外部手改文件后调用）
  fish::core::Error reload();

  // 扫描目录，把 manifest 里没有的图片自动纳入。
  // 用户直接把图丢进 portraits_dir 就能用，不必手写 manifest。
  // 返回新增数量。
  int scan_directory();

  [[nodiscard]] std::size_t size() const;

  // 挑一张：tag 非空且命中则优先，否则随机。
  // 库为空时返回空 Portrait。
  Portrait pick(const std::string& hint, PickStrategy strategy) const;

  // 按标签找（供模型主动查「有没有吃东西的图」）
  std::vector<Portrait> find_by_tag(const std::string& tag) const;

  // 追加一张（生成或用户上传后调用），同时落 manifest
  fish::core::Error add(const Portrait& p);

  // 全部（只读快照）
  std::vector<Portrait> all() const;

  [[nodiscard]] const std::string& dir() const noexcept { return dir_; }

 private:
  fish::core::Error load_manifest();
  fish::core::Error save_manifest() const;

  std::string dir_;
  mutable std::mutex mu_;
  std::vector<Portrait> items_;
};

// ── 实现 ────────────────────────────────────────────────

namespace fs = std::filesystem;

// 模块接口单元里不能有匿名 namespace（clang 报
// "anonymous namespaces cannot be exported"），
// 所以私有辅助放进具名 namespace detail，不 export 即可。
namespace detail {

std::string today_str() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
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

// manifest 行格式（人可手写）：
//   path | caption | tag1,tag2 | source
// 以 # 开头是注释
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

std::string trim_copy(std::string s) {
  const auto not_space = [](unsigned char c) {
    return c != ' ' && c != '\t' && c != '\r' && c != '\n';
  };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

}  // namespace detail

PortraitStore::PortraitStore(std::string dir) : dir_(std::move(dir)) {}

fish::core::Error PortraitStore::init() {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  if (ec) {
    return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                   "mkdir_portraits:" + ec.message());
  }
  return load_manifest();
}

fish::core::Error PortraitStore::reload() { return load_manifest(); }

int PortraitStore::scan_directory() {
  // 用户可以直接把图丢进 portraits 目录，不用手写 manifest。
  // 这里扫描常见图片扩展名，把 manifest 里没有的补进去，
  // caption 先用文件名，tags 留空（可手改 manifest 补充）。
  static const char* kExts[] = {".png", ".jpg", ".jpeg", ".gif", ".webp"};
  std::error_code ec;

  if (!fs::exists(dir_, ec)) return 0;

  std::vector<Portrait> added;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
      if (!entry.is_regular_file()) continue;
      const std::string path = entry.path().string();
      const std::string ext = entry.path().extension().string();

      bool ext_ok = false;
      for (const char* e : kExts) {
        if (ext == e) {
          ext_ok = true;
          break;
        }
      }
      if (!ext_ok) continue;

      // 已在 manifest 里就跳过
      bool known = false;
      for (const auto& p : items_) {
        if (p.path == path) {
          known = true;
          break;
        }
      }
      if (known) continue;

      Portrait p;
      p.path = path;
      p.caption = entry.path().stem().string();   // 用文件名做初始说明
      p.source = "scanned";
      p.created_at = detail::today_str();
      items_.push_back(p);
      added.push_back(p);
    }
  }

  if (!added.empty()) {
    auto e = save_manifest();
    if (!e.ok()) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "image", "manifest_save_failed")
          .field("detail", e.detail);
    }
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "image", "scanned")
        .field("added", static_cast<long long>(added.size()))
        .field("dir", dir_);
  }
  return static_cast<int>(added.size());
}

fish::core::Error PortraitStore::load_manifest() {
  const std::string path = dir_ + "/manifest.md";
  std::vector<Portrait> loaded;

  std::ifstream in(path);
  if (in) {
    std::string line;
    while (std::getline(in, line)) {
      line = detail::trim_copy(line);
      if (line.empty() || line[0] == '#') continue;
      const auto parts = detail::split(line, '|');
      if (parts.size() < 2) continue;

      Portrait p;
      p.path = detail::trim_copy(parts[0]);
      p.caption = detail::trim_copy(parts[1]);
      if (parts.size() > 2) {
        for (const auto& t : detail::split(parts[2], ',')) {
          const auto tag = detail::trim_copy(t);
          if (!tag.empty()) p.tags.push_back(tag);
        }
      }
      p.source = parts.size() > 3 ? detail::trim_copy(parts[3]) : "manual";
      p.created_at = parts.size() > 4 ? detail::trim_copy(parts[4]) : detail::today_str();
      if (!p.path.empty()) loaded.push_back(std::move(p));
    }
  }

  std::lock_guard<std::mutex> lock(mu_);
  items_ = std::move(loaded);

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "image", "portraits_loaded")
      .field("count", static_cast<long long>(items_.size()))
      .field("dir", dir_);
  return fish::core::Error::none();
}

fish::core::Error PortraitStore::save_manifest() const {
  const std::string path = dir_ + "/manifest.md";
  const std::string tmp = path + ".tmp";

  {
    std::ofstream out(tmp);
    if (!out) {
      return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                     "portraits_manifest_open");
    }
    out << "# 大肥鱼形象库\n";
    out << "# 格式: path | caption | tags | source | created_at\n";
    out << "# 直接手改这个文件即可增删图片，重启或 reload() 后生效。\n\n";
    for (const auto& p : items_) {
      std::string tags;
      for (std::size_t i = 0; i < p.tags.size(); ++i) {
        if (i > 0) tags += ",";
        tags += p.tags[i];
      }
      out << p.path << " | " << p.caption << " | " << tags << " | " << p.source
          << " | " << p.created_at << "\n";
    }
  }

  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                   "portraits_manifest_rename:" + ec.message());
  }
  return fish::core::Error::none();
}

std::size_t PortraitStore::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return items_.size();
}

std::vector<Portrait> PortraitStore::all() const {
  std::lock_guard<std::mutex> lock(mu_);
  return items_;
}

std::vector<Portrait> PortraitStore::find_by_tag(const std::string& tag) const {
  std::vector<Portrait> out;
  std::lock_guard<std::mutex> lock(mu_);
  if (items_.empty()) return out;

  // 标签双向子串匹配。模型传的是自然语言查询（"发张照片看看"），
  // 标签是短词（"吃,饭,日常"），能对上最好。
  if (!tag.empty()) {
    for (const auto& p : items_) {
      for (const auto& t : p.tags) {
        if (t.find(tag) != std::string::npos || tag.find(t) != std::string::npos) {
          out.push_back(p);
          break;
        }
      }
    }
  }

  // 一个都没匹配上就随机给一张。
  //
  // 为什么必须有这个兜底：用户说「发张照片看看」时模型传的 query 往往是
  // 「照片」「自拍」「你的样子」这类词，而形象库的标签是「吃」「饭」
  // 「日常」「表情」——实测 16 个典型 query 里有 7 个零命中。
  // 旧逻辑零命中就返回空，模型于是回「形象库里没有匹配的图片」，
  // 用户看到的是「没图片可发」。可用户要的就是「随便发张你的图」，
  // 库里有 129 张，随便挑一张完全满足需求。
  if (out.empty()) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> d(0, items_.size() - 1);
    out.push_back(items_[d(rng)]);
  }
  return out;
}

Portrait PortraitStore::pick(const std::string& hint, PickStrategy strategy) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (items_.empty()) return {};

  // 标签命中优先：有 hint 且匹配到，就在命中集合里挑
  if (strategy == PickStrategy::kTagMatch && !hint.empty()) {
    std::vector<std::size_t> hits;
    for (std::size_t i = 0; i < items_.size(); ++i) {
      for (const auto& t : items_[i].tags) {
        if (!t.empty() && hint.find(t) != std::string::npos) {
          hits.push_back(i);
          break;
        }
      }
    }
    if (!hits.empty()) {
      static thread_local std::mt19937 rng{std::random_device{}()};
      std::uniform_int_distribution<std::size_t> d(0, hits.size() - 1);
      return items_[hits[d(rng)]];
    }
  }

  static thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<std::size_t> d(0, items_.size() - 1);
  return items_[d(rng)];
}

fish::core::Error PortraitStore::add(const Portrait& p) {
  if (p.path.empty()) {
    return fish::core::Error::make(fish::core::ErrorCode::kBadRequest,
                                   "portrait_empty_path");
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& existing : items_) {
      if (existing.path == p.path) {
        return fish::core::Error::none();   // 已存在，幂等
      }
    }
    Portrait copy = p;
    if (copy.created_at.empty()) copy.created_at = detail::today_str();
    if (copy.source.empty()) copy.source = "generated";
    items_.push_back(std::move(copy));
  }
  return save_manifest();
}

}  // namespace fish::image
