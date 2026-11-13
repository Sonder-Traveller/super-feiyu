// src/fish.media.cppm
// 媒体缓存：把外部（腾讯 CDN 等）的图片/语音下载到本地，长期保留。
//
// 为什么需要：
//   QQ 等平台给的附件 URL 是**带签名的临时链接**，通常几小时到一天失效。
//   如果记忆里只存 URL，过几天就变成死链，模型"记得发过图但打不开"。
//   所以入站/出站媒体都落一份本地副本，记忆只引用本地路径。
//
// 设计要点（与记忆系统同构）：
//   - 内容是权威资产，按内容哈希命名，天然去重
//   - 目录结构按年月分桶，避免单目录文件过多
//   - 不做「按时间淘汰」：媒体是资产，不设时效
module;

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

export module fish.media;

import fish.core;
import fish.log;
import fish.capability;

export namespace fish::media {

// 一次落盘的元信息，写入 sidecar 便于人排查
struct Stored {
  std::string path;      // 本地绝对/相对路径
  std::string mime;
  std::size_t bytes{0};
  bool reused{false};    // 命中已有副本（内容哈希相同）
};

class MediaCache {
 public:
  struct Config {
    std::string dir{"data/memory/media"};
    std::size_t max_bytes{32u << 20};   // 单文件上限，防超大附件
    int timeout_ms{30000};
  };

  explicit MediaCache(Config cfg);

  // 建目录。幂等。
  fish::core::Error init();

  // 下载 http(s) URL 到本地缓存。命中相同内容则复用。
  // ext_hint 形如 ".png"，为空则按 mime 猜。
  fish::core::Result<Stored> fetch(const std::string& url,
                                   const std::string& ext_hint = "");

  // 把已有的本地文件纳入缓存（例如形象库里的图、生成的图）。
  // 已在该目录下的文件会直接复用，不重复拷贝。
  fish::core::Result<Stored> adopt(const std::string& local_path);

  // 把内存字节写入缓存（生成图 b64 落盘后调用）
  fish::core::Result<Stored> put(const std::string& bytes,
                                 const std::string& mime);

  [[nodiscard]] const std::string& dir() const noexcept { return cfg_.dir; }

 private:
  fish::core::Result<Stored> write_blob(const std::string& bytes,
                                        const std::string& mime,
                                        const std::string& ext);
  [[nodiscard]] std::string bucket_dir() const;

  Config cfg_;
  mutable std::mutex mu_;
};

// ── 实现 ────────────────────────────────────────────────

namespace fs = std::filesystem;

namespace detail {

// FNV-1a 64：内容寻址，去重靠它
std::string content_hash(const std::string& bytes) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const unsigned char c : bytes) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

std::string ext_from_mime(const std::string& mime) {
  static const std::map<std::string, std::string> kMap{
      {"image/png", ".png"},   {"image/jpeg", ".jpg"}, {"image/jpg", ".jpg"},
      {"image/gif", ".gif"},   {"image/webp", ".webp"},
      {"audio/mpeg", ".mp3"},  {"audio/mp3", ".mp3"},  {"audio/wav", ".wav"},
      {"audio/silk", ".silk"}, {"voice", ".silk"},     {"video/mp4", ".mp4"},
  };
  const auto it = kMap.find(mime);
  return it == kMap.end() ? ".bin" : it->second;
}

std::string now_bucket() {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);
  return buf;
}

}  // namespace detail

MediaCache::MediaCache(Config cfg) : cfg_(std::move(cfg)) {}

fish::core::Error MediaCache::init() {
  std::error_code ec;
  fs::create_directories(cfg_.dir, ec);
  if (ec) {
    return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                   "mkdir_media:" + ec.message());
  }
  return fish::core::Error::none();
}

std::string MediaCache::bucket_dir() const {
  return cfg_.dir + "/" + detail::now_bucket();
}

fish::core::Result<Stored> MediaCache::write_blob(const std::string& bytes,
                                                  const std::string& mime,
                                                  const std::string& ext) {
  if (bytes.empty()) {
    return fish::core::Result<Stored>::fail(fish::core::ErrorCode::kBadRequest,
                                            "media_empty");
  }
  if (bytes.size() > cfg_.max_bytes) {
    return fish::core::Result<Stored>::fail(
        fish::core::ErrorCode::kBadRequest,
        "media_too_large:" + std::to_string(bytes.size()));
  }

  const std::string suffix = ext.empty() ? detail::ext_from_mime(mime) : ext;
  const std::string name = detail::content_hash(bytes) + suffix;

  std::error_code ec;
  const std::string dir = bucket_dir();
  fs::create_directories(dir, ec);

  const std::string path = dir + "/" + name;
  Stored out;
  out.path = path;
  out.mime = mime;
  out.bytes = bytes.size();

  if (fs::exists(path, ec)) {
    out.reused = true;
    return fish::core::Result<Stored>::ok(std::move(out));
  }

  // 先写临时文件再 rename，避免读到写了一半的图
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary);
    if (!f) {
      return fish::core::Result<Stored>::fail(fish::core::ErrorCode::kInternal,
                                              "media_write");
    }
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    return fish::core::Result<Stored>::fail(fish::core::ErrorCode::kInternal,
                                            "media_rename:" + ec.message());
  }

  ::fish::log::log_line(::fish::log::LogLevel::kDebug, "media", "stored")
      .field("path", path)
      .field("bytes", static_cast<long long>(bytes.size()))
      .field("mime", mime);
  return fish::core::Result<Stored>::ok(std::move(out));
}

fish::core::Result<Stored> MediaCache::put(const std::string& bytes,
                                           const std::string& mime) {
  return write_blob(bytes, mime, "");
}

fish::core::Result<Stored> MediaCache::fetch(const std::string& url,
                                             const std::string& ext_hint) {
  // 非 http(s)（已是本地路径）直接纳入
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
    return adopt(url);
  }

  auto dl = fish::capability::HttpClient::instance().download(url, cfg_.max_bytes);
  if (!dl.ok()) {
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "media", "download_failed")
        .field("url", url.substr(0, 160))
        .field("detail", dl.error.detail);
    return fish::core::Result<Stored>::fail(dl.error.code, "media_download:" + dl.error.detail);
  }

  // 按 URL 后缀猜扩展名，猜不到就用 .bin（内容哈希仍能去重）
  std::string ext = ext_hint;
  if (ext.empty()) {
    const auto q = url.find('?');
    const std::string path = (q == std::string::npos) ? url : url.substr(0, q);
    const auto dot = path.find_last_of('.');
    if (dot != std::string::npos && path.size() - dot <= 6) {
      ext = path.substr(dot);
    }
  }
  return write_blob(*dl.value, "", ext);
}

fish::core::Result<Stored> MediaCache::adopt(const std::string& local_path) {
  std::error_code ec;
  if (!fs::exists(local_path, ec)) {
    return fish::core::Result<Stored>::fail(fish::core::ErrorCode::kBadRequest,
                                            "media_missing:" + local_path);
  }
  // 已经在缓存目录下，直接复用
  const std::string abs_local = fs::absolute(local_path, ec).string();
  const std::string abs_dir = fs::absolute(cfg_.dir, ec).string();
  if (abs_local.rfind(abs_dir, 0) == 0) {
    Stored s;
    s.path = local_path;
    s.bytes = static_cast<std::size_t>(fs::file_size(local_path, ec));
    s.reused = true;
    return fish::core::Result<Stored>::ok(std::move(s));
  }

  std::ifstream in(local_path, std::ios::binary | std::ios::ate);
  if (!in) {
    return fish::core::Result<Stored>::fail(fish::core::ErrorCode::kBadRequest,
                                            "media_open:" + local_path);
  }
  const auto size = in.tellg();
  if (size <= 0) {
    return fish::core::Result<Stored>::fail(fish::core::ErrorCode::kBadRequest,
                                            "media_empty");
  }
  in.seekg(0);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  in.read(bytes.data(), size);

  std::string ext;
  const auto dot = local_path.find_last_of('.');
  if (dot != std::string::npos && local_path.size() - dot <= 6) {
    ext = local_path.substr(dot);
  }
  return write_blob(bytes, "", ext);
}

}  // namespace fish::media
