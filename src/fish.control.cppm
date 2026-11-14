// src/fish.control.cppm
// 控制面后端：把「配置读写 / 扫码登录 / 渠道测试」做成可被 HTTP 层调用的服务。
//
// 为什么单独成模块：
//   fish.api 只该管 HTTP 协议（解析、鉴权、路由），不该知道 config.toml
//   的格式、二维码怎么画、q.qq.com 的扫码协议长什么样。那些是业务。
//   分开后 api 可以只 depend on control 的接口，不必把 OpenSSL/zlib 拖进来。
//
// 落盘的权威文件只有两个：
//   config.toml   非密钥配置，保留注释、只改目标行
//   .env.local    密钥，权限 600
// 密钥的判定是通用的：config.toml 里值形如 "${VAR}" 的键就是密钥。
module;

#include <zlib.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "qrcodegen.hpp"

export module fish.control;

import fish.core;
import fish.log;
import fish.capability;

export namespace fish::control {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── 二维码 ──────────────────────────────────────────────
//
// 由后端画成 PNG，前端只负责显示。
// 理由：各平台字体/抗锯齿差异会让同一条内容画出扫不出来的码，
// 后端出一张确定的图，手机扫得动是经过验证的。
std::string qr_png_base64(const std::string& text, int scale = 8, int margin = 4);

// 直接返回 PNG 字节（测试用，也便于落盘排查）
std::string qr_png_bytes(const std::string& text, int scale = 8, int margin = 4);

// ── config.toml 读写 ────────────────────────────────────
//
// 不重写整个文件：按行定位，只替换目标键的值。
// 注释、顺序、空行全部保留——用户手写的注释是资产，不能因为
// GUI 点了一下保存就全没了。
class ConfigFile {
 public:
  explicit ConfigFile(std::string path) : path_(std::move(path)) {}

  // 读盘。文件不存在返回空文档（不算错误）。
  fish::core::Error load();

  [[nodiscard]] std::optional<std::string> get(const std::string& section,
                                               const std::string& key) const;
  [[nodiscard]] std::string get_or(const std::string& section, const std::string& key,
                                   const std::string& def) const;

  // 该键是否是密钥占位符（值形如 "${VAR}"）。返回变量名。
  [[nodiscard]] std::optional<std::string> env_var_of(const std::string& section,
                                                      const std::string& key) const;

  // 设值。raw_value 是**未加引号**的字面值。
  // 已有的行就替换，没有就插到该 section 末尾，section 不存在就追加。
  void set(const std::string& section, const std::string& key, const std::string& raw_value);

  // 写盘：先写 .tmp 再 rename，失败不留半截文件。
  fish::core::Error save();

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  [[nodiscard]] std::size_t find_section_end(std::size_t section_start) const;
  [[nodiscard]] std::optional<std::size_t> find_section(const std::string& section) const;
  [[nodiscard]] std::optional<std::size_t> find_key(std::size_t section_line,
                                                     const std::string& key) const;

  std::string path_;
  std::vector<std::string> lines_;
};

// ── .env 读写 ───────────────────────────────────────────
class EnvFile {
 public:
  explicit EnvFile(std::string path) : path_(std::move(path)) {}

  fish::core::Error load();

  [[nodiscard]] std::optional<std::string> get(const std::string& name) const;

  // 设值。value 为空 = 删除该行（不是写空值，空值和不存在语义不同）。
  void set(const std::string& name, const std::string& value);

  fish::core::Error save();

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
  std::vector<std::string> lines_;
};

// ── 扫码任务 ────────────────────────────────────────────
//
// 服务端持有 task_id → 上次轮询结果。轮询是幂等的，但上游
// 可能因为网络抖动返回错误，缓存能让前端不会因为一次抖动就跳「失败」。
struct QqBindTask {
  std::string task_id;
  std::string key_b64;   // 解 AppSecret 用，只在服务端内存里
  std::string qr_content;
  std::int64_t created_ms{0};
};

struct WeChatLogin {
  std::string qrcode;
  std::int64_t created_ms{0};
};

// ── 控制面服务 ──────────────────────────────────────────
class ControlService {
 public:
  struct Config {
    std::string config_path{"config.toml"};
    std::string env_path{".env.local"};
    std::string data_dir{"data"};
  };

  explicit ControlService(Config cfg);

  // ── 配置 ──
  // 返回 GET /v1/config 的 JSON 文本。密钥只回 *_set。
  fish::core::Result<std::string> config_json();

  // 部分更新。changed 收集实际改动的点号路径。
  // restart_required 表示要重启进程才生效。
  fish::core::Error apply_config_json(const std::string& body,
                                      std::vector<std::string>* changed,
                                      bool* restart_required);

  // ── 渠道测试 ──
  fish::core::Result<std::string> qq_test(const std::string& app_id,
                                          const std::string& app_secret);
  fish::core::Result<std::string> wechat_test(const std::string& bot_token);

  // ── QQ 扫码绑定 ──
  fish::core::Result<std::string> qq_qr_create(const std::string& source);
  fish::core::Result<std::string> qq_qr_status(const std::string& task_id);

  // ── 微信扫码登录 ──
  fish::core::Result<std::string> wechat_qr_create();
  fish::core::Result<std::string> wechat_qr_status(const std::string& qrcode);

 private:
  // 把密钥写进 .env.local，并把 config.toml 里的值保持为 ${VAR}
  fish::core::Error set_secret(const std::string& section, const std::string& key,
                               const std::string& value);

  // 读当前生效的密钥值（config.toml 的 ${VAR} → .env.local → 进程环境）
  [[nodiscard]] std::string resolved(const std::string& section, const std::string& key);

  Config cfg_;
  std::mutex mu_;
  QqBindTask qq_task_;
  WeChatLogin wx_login_;
};

// ── 工具（导出便于测试）────────────────────────────────

std::string base64_encode(const std::string& in);
fish::core::Result<std::string> base64_decode(const std::string& in);

// AES-256-GCM 解密，布局 nonce(12) + ciphertext + tag(16)
fish::core::Result<std::string> aes_gcm_decrypt(const std::string& cipher_b64,
                                                const std::string& key_b64);

}  // namespace fish::control

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::control {

namespace {

std::string trim(const std::string& s) {
  const auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r'; };
  std::string out = s;
  out.erase(out.begin(), std::find_if(out.begin(), out.end(), not_space));
  out.erase(std::find_if(out.rbegin(), out.rend(), not_space).base(), out.end());
  return out;
}

std::string to_lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 只给「最近 N 秒」用，避免任务表无限增长
constexpr std::int64_t kTaskTtlMs = 6 * 60 * 1000;

// ── PNG 编码 ────────────────────────────────────────────
//
// 只需要「灰度 + 8bit + 无交错」这一种形态，够画二维码了。
void put_u32(std::string& out, std::uint32_t v) {
  out += static_cast<char>((v >> 24) & 0xFF);
  out += static_cast<char>((v >> 16) & 0xFF);
  out += static_cast<char>((v >> 8) & 0xFF);
  out += static_cast<char>(v & 0xFF);
}

void png_chunk(std::string& out, const char type[4], const std::string& data) {
  put_u32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t crc_start = out.size();
  out.append(type, 4);
  out += data;
  const auto crc = static_cast<std::uint32_t>(::crc32(
      0, reinterpret_cast<const Bytef*>(out.data() + crc_start), out.size() - crc_start));
  put_u32(out, crc);
}

}  // namespace

std::string base64_encode(const std::string& in) {
  static const char* kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  std::size_t i = 0;
  while (i + 2 < in.size()) {
    const auto n = (static_cast<unsigned char>(in[i]) << 16) |
                   (static_cast<unsigned char>(in[i + 1]) << 8) |
                   static_cast<unsigned char>(in[i + 2]);
    out += kTable[(n >> 18) & 63];
    out += kTable[(n >> 12) & 63];
    out += kTable[(n >> 6) & 63];
    out += kTable[n & 63];
    i += 3;
  }
  if (i < in.size()) {
    const auto b0 = static_cast<unsigned char>(in[i]);
    if (i + 1 < in.size()) {
      const auto b1 = static_cast<unsigned char>(in[i + 1]);
      const auto n = (b0 << 16) | (b1 << 8);
      out += kTable[(n >> 18) & 63];
      out += kTable[(n >> 12) & 63];
      out += kTable[(n >> 6) & 63];
      out += '=';
    } else {
      const auto n = (b0 << 16);
      out += kTable[(n >> 18) & 63];
      out += kTable[(n >> 12) & 63];
      out += "==";
    }
  }
  return out;
}

fish::core::Result<std::string> base64_decode(const std::string& in) {
  static const auto kIndex = [] {
    std::array<int, 256> t{};
    t.fill(-1);
    const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(table[i])] = i;
    return t;
  }();
  std::string out;
  out.reserve(in.size() / 4 * 3);
  int buf = 0;
  int bits = 0;
  for (const char c : in) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    const int v = kIndex[static_cast<unsigned char>(c)];
    if (v < 0) {
      return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kBadRequest,
                                                   "bad_base64");
    }
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((buf >> bits) & 0xFF);
    }
  }
  return fish::core::Result<std::string>::ok(std::move(out));
}

fish::core::Result<std::string> aes_gcm_decrypt(const std::string& cipher_b64,
                                                const std::string& key_b64) {
  auto key = base64_decode(key_b64);
  if (!key.ok()) return key;
  auto blob = base64_decode(cipher_b64);
  if (!blob.ok()) return blob;

  if (blob.value->size() < 12 + 16) {
    return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kBadRequest,
                                                 "cipher_too_short");
  }
  const auto& b = *blob.value;
  const std::string nonce = b.substr(0, 12);
  const std::string ct = b.substr(12, b.size() - 12 - 16);
  const std::string tag = b.substr(b.size() - 16);

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) {
    return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kInternal,
                                                 "evp_ctx_alloc");
  }
  std::string out(ct.size() + 16, '\0');
  int len = 0;
  int total = 0;
  bool ok = true;
  ok = ok && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
  ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                 static_cast<int>(nonce.size()), nullptr) == 1;
  ok = ok && EVP_DecryptInit_ex(
                 ctx, nullptr, nullptr,
                 reinterpret_cast<const unsigned char*>(key.value->data()),
                 reinterpret_cast<const unsigned char*>(nonce.data())) == 1;
  ok = ok && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()), &len,
                               reinterpret_cast<const unsigned char*>(ct.data()),
                               static_cast<int>(ct.size())) == 1;
  if (ok) total = len;
  ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                                 static_cast<int>(tag.size()),
                                 const_cast<char*>(tag.data())) == 1;
  if (ok) {
    ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + total,
                             &len) == 1;
    if (ok) total += len;
  }
  EVP_CIPHER_CTX_free(ctx);

  if (!ok) {
    // tag 校验失败就是密钥不对或数据被篡改，不能吞掉
    return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kUnauthorized,
                                                 "aes_gcm_tag_mismatch");
  }
  out.resize(static_cast<std::size_t>(total));
  return fish::core::Result<std::string>::ok(std::move(out));
}

std::string qr_png_bytes(const std::string& text, int scale, int margin) {
  if (scale < 1) scale = 1;
  if (margin < 0) margin = 0;

  const auto qr = qrcodegen::QrCode::encodeText(
      text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
  const int modules = qr.getSize();
  const int dim = (modules + margin * 2) * scale;

  // 每行：1 字节 filter(=0) + dim 字节灰度
  std::string raw;
  raw.reserve(static_cast<std::size_t>(dim) * (dim + 1));
  for (int y = 0; y < dim; ++y) {
    raw += '\0';
    const int my = y / scale - margin;
    for (int x = 0; x < dim; ++x) {
      const int mx = x / scale - margin;
      bool dark = false;
      if (mx >= 0 && mx < modules && my >= 0 && my < modules) {
        dark = qr.getModule(mx, my);
      }
      raw += dark ? '\0' : static_cast<char>(0xFF);
    }
  }

  // zlib 压缩（PNG 的 IDAT 要 zlib 流，不是裸 deflate）
  uLongf bound = compressBound(static_cast<uLong>(raw.size()));
  std::string z(bound, '\0');
  if (::compress2(reinterpret_cast<Bytef*>(z.data()), &bound,
                  reinterpret_cast<const Bytef*>(raw.data()),
                  static_cast<uLong>(raw.size()), 9) != Z_OK) {
    return {};
  }
  z.resize(bound);

  std::string png;
  png += "\x89PNG\r\n\x1a\n";
  {
    std::string ihdr;
    put_u32(ihdr, static_cast<std::uint32_t>(dim));
    put_u32(ihdr, static_cast<std::uint32_t>(dim));
    ihdr += static_cast<char>(8);   // bit depth
    ihdr += static_cast<char>(0);   // color type: grayscale
    ihdr += static_cast<char>(0);   // compression
    ihdr += static_cast<char>(0);   // filter
    ihdr += static_cast<char>(0);   // interlace
    png_chunk(png, "IHDR", ihdr);
  }
  png_chunk(png, "IDAT", z);
  png_chunk(png, "IEND", {});
  return png;
}

std::string qr_png_base64(const std::string& text, int scale, int margin) {
  return base64_encode(qr_png_bytes(text, scale, margin));
}

// ── ConfigFile ──────────────────────────────────────────

fish::core::Error ConfigFile::load() {
  lines_.clear();
  std::ifstream in(path_);
  if (!in) {
    // 首次运行时可能还没有 config.toml，建空的即可
    return fish::core::Error::none();
  }
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines_.push_back(line);
  }
  return fish::core::Error::none();
}

std::optional<std::size_t> ConfigFile::find_section(const std::string& section) const {
  const std::string want = "[" + section + "]";
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (trim(lines_[i]) == want) return i;
  }
  return std::nullopt;
}

std::size_t ConfigFile::find_section_end(std::size_t section_start) const {
  for (std::size_t i = section_start + 1; i < lines_.size(); ++i) {
    const auto t = trim(lines_[i]);
    if (!t.empty() && t.front() == '[' && t.back() == ']') return i;
  }
  return lines_.size();
}

std::optional<std::size_t> ConfigFile::find_key(std::size_t section_line,
                                                const std::string& key) const {
  const std::size_t end = find_section_end(section_line);
  for (std::size_t i = section_line + 1; i < end; ++i) {
    const std::string t = trim(lines_[i]);
    if (t.empty() || t.front() == '#') continue;
    const auto eq = t.find('=');
    if (eq == std::string::npos) continue;
    if (trim(t.substr(0, eq)) == key) return i;
  }
  return std::nullopt;
}

std::optional<std::string> ConfigFile::get(const std::string& section,
                                           const std::string& key) const {
  const auto s = find_section(section);
  if (!s) return std::nullopt;
  const auto k = find_key(*s, key);
  if (!k) return std::nullopt;

  std::string v = trim(lines_[*k]);
  const auto eq = v.find('=');
  if (eq == std::string::npos) return std::nullopt;
  v = trim(v.substr(eq + 1));

  // 去引号；顺便剥掉行尾注释（引号内的 # 不剥）
  if (!v.empty() && (v.front() == '"' || v.front() == '\'')) {
    const char q = v.front();
    const auto close = v.find(q, 1);
    if (close != std::string::npos) return v.substr(1, close - 1);
    return v.substr(1);
  }
  const auto hash = v.find('#');
  if (hash != std::string::npos) v = trim(v.substr(0, hash));
  return v;
}

std::string ConfigFile::get_or(const std::string& section, const std::string& key,
                               const std::string& def) const {
  auto v = get(section, key);
  if (!v || v->empty()) return def;
  return *v;
}

std::optional<std::string> ConfigFile::env_var_of(const std::string& section,
                                                  const std::string& key) const {
  auto v = get(section, key);
  if (!v) return std::nullopt;
  const auto& s = *v;
  if (s.size() > 4 && s[0] == '$' && s[1] == '{' && s.back() == '}') {
    return s.substr(2, s.size() - 3);
  }
  return std::nullopt;
}

void ConfigFile::set(const std::string& section, const std::string& key,
                     const std::string& raw_value) {
  // 值一律加双引号：本项目用到的配置项全是字符串/数字/bool，
  // 数字和 bool 加引号也能被 load() 的 unquote + get_int_or/get_bool_or 正确处理。
  const std::string line = key + " = \"" + raw_value + "\"";

  auto s = find_section(section);
  if (!s) {
    if (!lines_.empty() && !lines_.back().empty()) lines_.push_back("");
    lines_.push_back("[" + section + "]");
    lines_.push_back(line);
    return;
  }
  auto k = find_key(*s, key);
  if (k) {
    lines_[*k] = line;
    return;
  }
  // 插在 section 末尾；跳过尾部空行，让新键紧贴上一个键
  std::size_t end = find_section_end(*s);
  while (end > *s + 1 && trim(lines_[end - 1]).empty()) --end;
  lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(end), line);
}

fish::core::Error ConfigFile::save() {
  // 原位写入 + 写前备份。
  //
  // 为什么不是「写 .tmp 再 rename」：
  //   systemd ProtectSystem=strict 用 bind mount 放行单个文件时，
  //   rename 替换操作的是父目录（unlink + link），目录没放行 → EROFS；
  //   tmp 放 data/ 再 rename 又跨 bind 挂载 → EXDEV。两条路都死。
  //   文件级白名单只支持原位写。
  //
  // 原子性补偿：写前把旧内容备份到 data/config.toml.bak（data/ 可写），
  // 写失败时留有完整旧版可人工恢复。
  {
    // 1) 备份旧文件（失败不阻塞写：备份只是补偿措施）
    std::error_code ec;
    const auto parent = fs::path(path_).parent_path();
    const auto bak_dir = parent / "data";
    if (fs::exists(bak_dir, ec)) {
      std::ifstream src(path_, std::ios::binary);
      if (src) {
        std::ofstream bak(bak_dir / "config.toml.bak",
                          std::ios::binary | std::ios::trunc);
        bak << src.rdbuf();
      }
    }

    // 2) 原位截断重写
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) {
      return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                     "config_write_failed:" + path_);
    }
    for (const auto& l : lines_) out << l << '\n';
    out.flush();
    if (!out) {
      return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                     "config_write_failed:" + path_);
    }
  }
  return fish::core::Error::none();
}

// ── EnvFile ─────────────────────────────────────────────

fish::core::Error EnvFile::load() {
  lines_.clear();
  std::ifstream in(path_);
  if (!in) return fish::core::Error::none();
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines_.push_back(line);
  }
  return fish::core::Error::none();
}

std::optional<std::string> EnvFile::get(const std::string& name) const {
  const std::string prefix = name + "=";
  for (const auto& l : lines_) {
    const auto t = trim(l);
    if (t.empty() || t.front() == '#') continue;
    if (t.rfind(prefix, 0) == 0) return t.substr(prefix.size());
  }
  return std::nullopt;
}

void EnvFile::set(const std::string& name, const std::string& value) {
  const std::string prefix = name + "=";
  std::vector<std::string> kept;
  kept.reserve(lines_.size() + 1);
  bool replaced = false;
  for (const auto& l : lines_) {
    const auto t = trim(l);
    if (!t.empty() && t.front() != '#' && t.rfind(prefix, 0) == 0) {
      if (value.empty()) {
        // 删掉这一行（清除密钥），不要留 "NAME=" 的空壳
        replaced = true;
        continue;
      }
      kept.push_back(prefix + value);
      replaced = true;
      continue;
    }
    kept.push_back(l);
  }
  if (!replaced && !value.empty()) kept.push_back(prefix + value);
  lines_ = std::move(kept);
}

fish::core::Error EnvFile::save() {
  // 原位写入（理由同 ConfigFile::save：文件级白名单不支持 rename 替换）。
  // 写前备份到 data/.env.local.bak。写后强制 600。
  {
    std::error_code ec;
    const auto parent = fs::path(path_).parent_path();
    const auto bak_dir = parent / "data";
    if (fs::exists(bak_dir, ec)) {
      std::ifstream src(path_, std::ios::binary);
      if (src) {
        std::ofstream bak(bak_dir / ".env.local.bak",
                          std::ios::binary | std::ios::trunc);
        bak << src.rdbuf();
      }
    }

    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) {
      return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                     "env_write_failed:" + path_);
    }
    for (const auto& l : lines_) out << l << '\n';
    out.flush();
    if (!out) {
      return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                     "env_write_failed:" + path_);
    }
  }
  std::error_code ec;
  // 密钥文件必须 600：同机其他用户读不到
  fs::permissions(path_, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace, ec);
  return fish::core::Error::none();
}

// ═══ ControlService ═════════════════════════════════════

ControlService::ControlService(Config cfg) : cfg_(std::move(cfg)) {}

std::string ControlService::resolved(const std::string& section, const std::string& key) {
  ConfigFile cf(cfg_.config_path);
  cf.load();
  // 1) config.toml 里是 ${VAR} 占位符 → 去 .env.local / 环境变量找
  if (auto var = cf.env_var_of(section, key)) {
    EnvFile ef(cfg_.env_path);
    ef.load();
    if (auto v = ef.get(*var)) return *v;
    if (const char* e = std::getenv(var->c_str()); e != nullptr) return e;
    return {};
  }
  // 2) 直接写字面值 → 用它
  return cf.get_or(section, key, "");
}

fish::core::Error ControlService::set_secret(const std::string& section,
                                              const std::string& key,
                                              const std::string& value) {
  ConfigFile cf(cfg_.config_path);
  if (auto e = cf.load(); !e.ok()) return e;

  // 变量名优先取 config.toml 里已声明的 ${VAR}；没有就按惯例生成
  std::string var;
  if (auto v = cf.env_var_of(section, key)) {
    var = *v;
  } else {
    // section.key → SECTION_KEY 大写。qq_official.app_secret → QQ_OFFICIAL_APP_SECRET
    var = section + "." + key;
    for (auto& c : var) {
      if (c == '.' || c == '-') c = '_';
      else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    // config.toml 里的值改成 ${VAR}，从此它就是「密钥」而不是明文字段
    cf.set(section, key, "${" + var + "}");
    if (auto e = cf.save(); !e.ok()) return e;
  }

  EnvFile ef(cfg_.env_path);
  if (auto e = ef.load(); !e.ok()) return e;
  ef.set(var, value);
  return ef.save();
}

// ── 配置视图 ────────────────────────────────────────────
//
// 每项：(section, key, 类型)。类型决定 JSON 里的形态。
// 密钥项的 key 后缀自动是 "_set"，值是「是否非空」。
namespace {

enum class FieldKind { kString, kInt, kBool, kSecret };

struct FieldDef {
  const char* section;
  const char* key;
  FieldKind kind;
};

constexpr FieldDef kFields[] = {
    // 通道
    {"channel.qq_official", "enabled", FieldKind::kBool},
    {"channel.qq_official", "app_id", FieldKind::kString},
    {"channel.qq_official", "app_secret", FieldKind::kSecret},
    {"channel.qq_onebot", "enabled", FieldKind::kBool},
    {"channel.qq_onebot", "ws_url", FieldKind::kString},
    {"channel.qq_onebot", "http_url", FieldKind::kString},
    {"channel.qq_onebot", "self_id", FieldKind::kString},
    {"channel.qq_onebot", "access_token", FieldKind::kSecret},
    {"channel.wechat", "enabled", FieldKind::kBool},
    {"channel.wechat", "bot_token", FieldKind::kSecret},
    {"channel.wechat", "bot_base_url", FieldKind::kString},
    // 能力
    {"capability.chat", "provider", FieldKind::kString},
    {"capability.chat", "base_url", FieldKind::kString},
    {"capability.chat", "model", FieldKind::kString},
    {"capability.chat", "reasoning_effort", FieldKind::kString},
    {"capability.chat", "api_key", FieldKind::kSecret},
    {"capability.embedding", "provider", FieldKind::kString},
    {"capability.embedding", "base_url", FieldKind::kString},
    {"capability.embedding", "model", FieldKind::kString},
    {"capability.embedding", "api_key", FieldKind::kSecret},
    {"capability.stt", "provider", FieldKind::kString},
    {"capability.stt", "base_url", FieldKind::kString},
    {"capability.stt", "model", FieldKind::kString},
    {"capability.stt", "language", FieldKind::kString},
    {"capability.stt", "api_key", FieldKind::kSecret},
    {"capability.tts", "provider", FieldKind::kString},
    {"capability.tts", "voice", FieldKind::kString},
    {"capability.tts", "format", FieldKind::kString},
    {"capability.tts", "group_voice", FieldKind::kBool},
    {"capability.search", "provider", FieldKind::kString},
    {"capability.search", "base_url", FieldKind::kString},
    {"capability.search", "api_key", FieldKind::kSecret},
    {"capability.search", "max_results", FieldKind::kInt},
    {"capability.image", "provider", FieldKind::kString},
    {"capability.image", "base_url", FieldKind::kString},
    {"capability.image", "model", FieldKind::kString},
    // 其它
    {"api", "port", FieldKind::kInt},
    {"api", "bind", FieldKind::kString},
    {"api", "auth", FieldKind::kBool},
    {"memory", "model_context_tokens", FieldKind::kInt},
    {"memory", "portraits_dir", FieldKind::kString},
    {"memory", "db_path", FieldKind::kString},
    {"probe", "ttl_days", FieldKind::kInt},
    {"probe", "on_start", FieldKind::kBool},
    {"tool", "max_rounds", FieldKind::kInt},
    {"guard", "max_input_bytes", FieldKind::kInt},
    {"guard", "max_output_bytes", FieldKind::kInt},
    {"guard", "rate_limit", FieldKind::kInt},
    {"guard", "rate_window_s", FieldKind::kInt},
    {"log", "level", FieldKind::kString},
};

// "channel.qq_official" → ["channel","qq_official"]；"api" → ["api",""]
std::pair<std::string, std::string> split_section(const std::string& full) {
  const auto dot = full.rfind('.');
  if (dot == std::string::npos) return {full, ""};
  return {full.substr(0, dot), full.substr(dot + 1)};
}

// 把嵌套 JSON 摊平成 "a.b.c" → value 的表。
// PUT 的请求体和 GET 同构（section 可以是两段），不摊平就得为每种
// 深度写一遍循环。
void flatten_json(const std::string& prefix, const json& v,
                  std::vector<std::pair<std::string, json>>* out) {
  if (v.is_object()) {
    for (const auto& [k, child] : v.items()) {
      flatten_json(prefix.empty() ? k : prefix + "." + k, child, out);
    }
    return;
  }
  out->emplace_back(prefix, v);
}

}  // namespace

fish::core::Result<std::string> ControlService::config_json() {
  std::lock_guard<std::mutex> lock(mu_);
  ConfigFile cf(cfg_.config_path);
  cf.load();

  json root = json::object();
  for (const auto& f : kFields) {
    // config.toml 的 section 可能是两段（channel.qq_official），
    // JSON 里要落成嵌套：root["channel"]["qq_official"]["enabled"]。
    // 早期版本用 rfind('.') 一刀切，结果变成 root["channel"]["enabled"] 加
    // 一个多余的 "qq_official" 键，前端读不到任何东西。
    std::string path{f.section};
    std::vector<std::string> segs;
    std::size_t start = 0;
    while (true) {
      const auto dot = path.find('.', start);
      if (dot == std::string::npos) {
        segs.push_back(path.substr(start));
        break;
      }
      segs.push_back(path.substr(start, dot - start));
      start = dot + 1;
    }

    json* node = &root;
    for (const auto& s : segs) node = &((*node)[s]);

    switch (f.kind) {
      case FieldKind::kString: {
        // 字符串值若是 ${VAR} 占位符，返回解析后的实际值。
        // 直接暴露 "${QQ_APP_ID}" 原文会让 GUI 把占位符当成配置内容
        // 展示/回写，回写一次就把 ${VAR} 变成死字符串，环境变量从此失效。
        const auto raw = cf.get(f.section, f.key);
        if (raw && !raw->empty() && raw->size() > 4 && (*raw)[0] == '$' &&
            (*raw)[1] == '{' && raw->back() == '}') {
          (*node)[f.key] = resolved(f.section, f.key);
        } else {
          (*node)[f.key] = raw.value_or("");
        }
        break;
      }
      case FieldKind::kInt: {
        const auto raw = cf.get_or(f.section, f.key, "0");
        long long v = 0;
        try {
          v = std::stoll(raw);
        } catch (...) {
        }
        (*node)[f.key] = v;
        break;
      }
      case FieldKind::kBool: {
        const auto raw = to_lower(cf.get_or(f.section, f.key, "false"));
        (*node)[f.key] = (raw == "true" || raw == "1" || raw == "yes" || raw == "on");
        break;
      }
      case FieldKind::kSecret:
        (*node)[std::string(f.key) + "_set"] = !resolved(f.section, f.key).empty();
        break;
    }
  }
  return fish::core::Result<std::string>::ok(root.dump());
}

fish::core::Error ControlService::apply_config_json(const std::string& body,
                                                    std::vector<std::string>* changed,
                                                    bool* restart_required) {
  json in;
  try {
    in = json::parse(body);
  } catch (const std::exception& e) {
    return fish::core::Error::make(fish::core::ErrorCode::kBadRequest,
                                   std::string("bad_json:") + e.what());
  }
  if (!in.is_object()) {
    return fish::core::Error::make(fish::core::ErrorCode::kBadRequest, "body_not_object");
  }

  std::lock_guard<std::mutex> lock(mu_);
  ConfigFile cf(cfg_.config_path);
  if (auto e = cf.load(); !e.ok()) return e;

  bool dirty = false;
  bool restart = false;

  // 摊平成 "channel.qq_official.enabled" → value，跟 kFields 的
  // "section.key" 对得上（section 本身可能带点）。
  std::vector<std::pair<std::string, json>> flat;
  flatten_json("", in, &flat);

  for (const auto& [full, val] : flat) {
    const FieldDef* def = nullptr;
    for (const auto& f : kFields) {
      if (full == f.section + std::string(".") + f.key) {
        def = &f;
        break;
      }
    }
    if (def == nullptr) {
      // 不静默忽略：用户以为改成功了最难查
      return fish::core::Error::make(fish::core::ErrorCode::kBadRequest,
                                     "unknown_field:" + full);
    }

    // 密钥三态：__keep__ 不动，空串清除，其余覆盖
    if (def->kind == FieldKind::kSecret) {
      if (!val.is_string()) {
        return fish::core::Error::make(fish::core::ErrorCode::kBadRequest,
                                       "secret_not_string:" + full);
      }
      const std::string v = val.get<std::string>();
      if (v == "__keep__") continue;
      if (auto e = set_secret(def->section, def->key, v); !e.ok()) return e;
      changed->push_back(full);
      dirty = true;
      restart = true;
      continue;
    }

    std::string text;
    switch (def->kind) {
      case FieldKind::kString:
        if (!val.is_string()) {
          return fish::core::Error::make(fish::core::ErrorCode::kBadRequest,
                                         "not_string:" + full);
        }
        text = val.get<std::string>();
        break;
      case FieldKind::kInt:
        if (!val.is_number_integer()) {
          return fish::core::Error::make(fish::core::ErrorCode::kBadRequest,
                                         "not_integer:" + full);
        }
        text = std::to_string(val.get<long long>());
        break;
      case FieldKind::kBool:
        if (!val.is_boolean()) {
          return fish::core::Error::make(fish::core::ErrorCode::kBadRequest,
                                         "not_bool:" + full);
        }
        text = val.get<bool>() ? "true" : "false";
        break;
      case FieldKind::kSecret:
        break;  // 上面已处理
    }

    // config.toml 里是 ${VAR} 占位符的键，写回时只有值真的变了才落盘。
    // GUI 会把 GET 到的解析值原样 PUT 回来；如果和当前解析值一致，
    // 绝不能把占位符覆盖成字面值，否则环境变量引用被杀死。
    if (def->kind == FieldKind::kString && cf.env_var_of(def->section, def->key)) {
      if (resolved(def->section, def->key) == text) continue;
    }

    if (cf.get_or(def->section, def->key, "") == text) continue;  // 无变化不记
    cf.set(def->section, def->key, text);
    changed->push_back(full);
    dirty = true;
    // 通道启停、模型、探测都需要重启才生效；纯展示项不用
    const std::string s{def->section};
    if (s.rfind("channel.", 0) == 0 || s.rfind("capability.", 0) == 0 ||
        s == "api" || s == "probe" || s == "tool") {
      restart = true;
    }
  }

  if (dirty) {
    if (auto e = cf.save(); !e.ok()) return e;
  }
  if (restart_required != nullptr) *restart_required = restart;
  return fish::core::Error::none();
}

// ── 渠道测试 ────────────────────────────────────────────

namespace {

// 换 QQ access_token。返回 (token, 错误详情)
fish::core::Result<std::string> qq_access_token(const std::string& app_id,
                                                const std::string& app_secret) {
  json body{{"appId", app_id}, {"clientSecret", app_secret}};
  auto r = fish::capability::HttpClient::instance().post_json(
      "https://bots.qq.com/app/getAppAccessToken", {{"Content-Type", "application/json"}},
      body.dump(), 15000);
  if (!r.ok()) {
    return fish::core::Result<std::string>::fail(r.error.code, r.error.detail);
  }
  try {
    const auto j = json::parse(r.value->body);
    if (j.contains("access_token")) {
      return fish::core::Result<std::string>::ok(j["access_token"].get<std::string>());
    }
    // 100007 = appid invalid；100016 = secret 不对
    const std::string code = j.value("code", -1) == -1 ? "?" : std::to_string(j["code"].get<int>());
    return fish::core::Result<std::string>::fail(
        fish::core::ErrorCode::kUnauthorized,
        "token_endpoint:" + code + ":" + j.value("message", ""));
  } catch (const std::exception& e) {
    return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kUpstreamUnavailable,
                                                 std::string("token_parse:") + e.what());
  }
}

}  // namespace

fish::core::Result<std::string> ControlService::qq_test(const std::string& app_id,
                                                        const std::string& app_secret) {
  const std::string id = app_id.empty() ? resolved("channel.qq_official", "app_id") : app_id;
  const std::string secret =
      app_secret.empty() ? resolved("channel.qq_official", "app_secret") : app_secret;

  json out;
  if (id.empty() || secret.empty()) {
    out["ok"] = false;
    out["error"] = "missing_credentials";
    out["detail"] = "AppID 或 AppSecret 为空";
    return fish::core::Result<std::string>::ok(out.dump());
  }

  auto tok = qq_access_token(id, secret);
  if (!tok.ok()) {
    out["ok"] = false;
    out["error"] = "invalid_credentials";
    out["detail"] = tok.error.detail;
    return fish::core::Result<std::string>::ok(out.dump());
  }

  // 拿 /users/@me 验证权限真的开通了（只有 token 不代表接口能调）
  auto me = fish::capability::HttpClient::instance().perform(
      {.method = "GET",
       .url = "https://api.sgroup.qq.com/users/@me",
       .headers = {{"Authorization", "QQBot " + *tok.value},
                   {"Content-Type", "application/json"}},
       .timeout_ms = 15000});
  if (!me.ok()) {
    out["ok"] = false;
    out["error"] = "api_unreachable";
    out["detail"] = me.error.detail;
    return fish::core::Result<std::string>::ok(out.dump());
  }
  try {
    const auto j = json::parse(me.value->body);
    out["ok"] = me.value->status == 200;
    if (out["ok"].get<bool>()) {
      out["bot_name"] = j.value("username", "");
      out["bot_uin"] = j.value("id", "");
      out["share_url"] = j.value("share_url", "");
    } else {
      out["error"] = "api_error";
      out["detail"] = "HTTP " + std::to_string(me.value->status) + ": " +
                      j.value("message", me.value->body.substr(0, 160));
    }
  } catch (const std::exception& e) {
    out["ok"] = false;
    out["error"] = "parse_failed";
    out["detail"] = e.what();
  }
  return fish::core::Result<std::string>::ok(out.dump());
}

fish::core::Result<std::string> ControlService::wechat_test(const std::string& bot_token) {
  const std::string tok =
      bot_token.empty() ? resolved("channel.wechat", "bot_token") : bot_token;

  json out;
  if (tok.empty()) {
    out["ok"] = false;
    out["error"] = "missing_credentials";
    out["detail"] = "bot_token 为空";
    return fish::core::Result<std::string>::ok(out.dump());
  }

  // getconfig 是唯一「带 token 且会立刻返回」的端点，用它验证 token 有效。
  // getupdates 会 hold 30 秒，不适合做连通性测试。
  auto r = fish::capability::HttpClient::instance().post_json(
      "https://ilinkai.weixin.qq.com/ilink/bot/getconfig",
      {{"Content-Type", "application/json"},
       {"AuthorizationType", "ilink_bot_token"},
       {"Authorization", "Bearer " + tok}},
      "{}", 15000);
  if (!r.ok()) {
    out["ok"] = false;
    out["error"] = "api_unreachable";
    out["detail"] = r.error.detail;
    return fish::core::Result<std::string>::ok(out.dump());
  }
  try {
    const auto j = json::parse(r.value->body);
    // errcode -14 = session timeout（token 失效）
    const int ec = j.value("errcode", 0);
    if (ec == 0) {
      out["ok"] = true;
      out["bot_id"] = j.value("bot_id", "");
    } else {
      out["ok"] = false;
      out["error"] = "invalid_token";
      out["detail"] = "errcode " + std::to_string(ec) + ": " + j.value("errmsg", "");
    }
  } catch (const std::exception& e) {
    out["ok"] = false;
    out["error"] = "parse_failed";
    out["detail"] = e.what();
  }
  return fish::core::Result<std::string>::ok(out.dump());
}

// ── QQ 扫码 ─────────────────────────────────────────────

fish::core::Result<std::string> ControlService::qq_qr_create(const std::string& source) {
  // key 是 32 字节随机数，base64 后给上游；解 AppSecret 也用它
  std::string key_raw(32, '\0');
  if (FILE* f = std::fopen("/dev/urandom", "rb")) {
    const auto n = std::fread(key_raw.data(), 1, key_raw.size(), f);
    std::fclose(f);
    if (n != key_raw.size()) key_raw.resize(n);
  }
  if (key_raw.size() != 32) {
    // 兜底：用时间 + 地址搅一下。真的到这一步说明 urandom 不可用，
    // 安全性已经打折，但总比崩溃好，日志里会看到。
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "control", "urandom_failed");
    std::uint64_t a = static_cast<std::uint64_t>(now_ms());
    for (std::size_t i = 0; i < 32; ++i) key_raw[i] = static_cast<char>((a >> ((i % 8) * 8)) ^ i);
  }
  const std::string key_b64 = base64_encode(key_raw);

  json body{{"key", key_b64}};
  auto r = fish::capability::HttpClient::instance().post_json(
      "https://q.qq.com/lite/create_bind_task",
      {{"Content-Type", "application/json"}, {"Accept", "application/json"}},
      body.dump(), 15000);
  if (!r.ok()) {
    return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kUpstreamUnavailable,
                                                 r.error.detail);
  }

  std::string task_id;
  try {
    const auto j = json::parse(r.value->body);
    if (j.value("retcode", -1) != 0) {
      return fish::core::Result<std::string>::fail(
          fish::core::ErrorCode::kUpstreamUnavailable,
          "create_bind_task:" + std::to_string(j.value("retcode", -1)) + ":" +
              j.value("msg", ""));
    }
    task_id = j["data"].value("task_id", "");
  } catch (const std::exception& e) {
    return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kUpstreamUnavailable,
                                                 std::string("parse:") + e.what());
  }
  if (task_id.empty()) {
    return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kUpstreamUnavailable,
                                                 "missing_task_id");
  }

  const std::string content =
      "https://q.qq.com/qqbot/openclaw/connect.html?task_id=" + task_id +
      "&source=" + (source.empty() ? std::string("super-feiyu") : source) + "&_wv=2";

  {
    std::lock_guard<std::mutex> lock(mu_);
    qq_task_ = QqBindTask{task_id, key_b64, content, now_ms()};
  }

  json out;
  out["task_id"] = task_id;
  out["expires_in"] = 300;
  out["qr_content"] = content;
  out["qr_png_b64"] = qr_png_base64(content, 8, 3);
  return fish::core::Result<std::string>::ok(out.dump());
}

fish::core::Result<std::string> ControlService::qq_qr_status(const std::string& task_id) {
  // 任务只在内存里；进程重启或等太久都要让前端重新扫码
  std::string key_b64;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (qq_task_.task_id != task_id || task_id.empty()) {
      json out{{"status", "expired"}};
      return fish::core::Result<std::string>::ok(out.dump());
    }
    if (now_ms() - qq_task_.created_ms > kTaskTtlMs) {
      json out{{"status", "expired"}};
      return fish::core::Result<std::string>::ok(out.dump());
    }
    key_b64 = qq_task_.key_b64;
  }

  json body{{"task_id", task_id}};
  auto r = fish::capability::HttpClient::instance().post_json(
      "https://q.qq.com/lite/poll_bind_result",
      {{"Content-Type", "application/json"}, {"Accept", "application/json"}},
      body.dump(), 15000);
  if (!r.ok()) {
    json out{{"status", "error"}, {"detail", r.error.detail}};
    return fish::core::Result<std::string>::ok(out.dump());
  }

  try {
    const auto j = json::parse(r.value->body);
    if (j.value("retcode", -1) != 0) {
      json out{{"status", "error"},
               {"detail", std::to_string(j.value("retcode", -1)) + ":" + j.value("msg", "")}};
      return fish::core::Result<std::string>::ok(out.dump());
    }
    const auto& d = j["data"];
    const int st = d.value("status", 0);

    if (st == 2) {  // COMPLETED
      const std::string app_id = d.value("bot_appid", "");
      const std::string enc = d.value("bot_encrypt_secret", "");
      auto secret = aes_gcm_decrypt(enc, key_b64);
      if (!secret.ok()) {
        json out{{"status", "error"}, {"detail", "decrypt_failed:" + secret.error.detail}};
        return fish::core::Result<std::string>::ok(out.dump());
      }

      // 扫码成功即落盘：用户扫完就该能用，不该再让他手抄两串密钥
      if (auto e = set_secret("channel.qq_official", "app_id", app_id); !e.ok()) {
        json out{{"status", "error"}, {"detail", "save_app_id:" + e.detail}};
        return fish::core::Result<std::string>::ok(out.dump());
      }
      if (auto e = set_secret("channel.qq_official", "app_secret", *secret.value); !e.ok()) {
        json out{{"status", "error"}, {"detail", "save_secret:" + e.detail}};
        return fish::core::Result<std::string>::ok(out.dump());
      }
      {
        ConfigFile cf(cfg_.config_path);
        cf.load();
        cf.set("channel.qq_official", "enabled", "true");
        if (auto e = cf.save(); !e.ok()) {
          ::fish::log::log_line(::fish::log::LogLevel::kWarn, "control", "enable_qq_failed")
              .field("detail", e.detail);
        }
      }

      json out{{"status", "completed"}, {"app_id", app_id}, {"saved", true}};
      if (d.contains("user_openid")) out["openid"] = d["user_openid"];
      return fish::core::Result<std::string>::ok(out.dump());
    }
    if (st == 3) {  // EXPIRED
      json out{{"status", "expired"}};
      return fish::core::Result<std::string>::ok(out.dump());
    }
    // 0 NONE / 1 PENDING
    json out{{"status", "pending"}};
    return fish::core::Result<std::string>::ok(out.dump());
  } catch (const std::exception& e) {
    json out{{"status", "error"}, {"detail", e.what()}};
    return fish::core::Result<std::string>::ok(out.dump());
  }
}

// ── 微信扫码 ────────────────────────────────────────────

fish::core::Result<std::string> ControlService::wechat_qr_create() {
  auto r = fish::capability::HttpClient::instance().perform(
      {.method = "GET",
       .url = "https://ilinkai.weixin.qq.com/ilink/bot/get_bot_qrcode?bot_type=3",
       .timeout_ms = 15000});
  if (!r.ok()) {
    return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kUpstreamUnavailable,
                                                 r.error.detail);
  }
  try {
    const auto j = json::parse(r.value->body);
    if (j.value("ret", -1) != 0) {
      return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kUpstreamUnavailable,
                                                   "get_bot_qrcode ret!=0");
    }
    const std::string qrcode = j.value("qrcode", "");
    const std::string content = j.value("qrcode_img_content", "");
    if (qrcode.empty() || content.empty()) {
      return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kUpstreamUnavailable,
                                                   "missing_qrcode_fields");
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      wx_login_ = WeChatLogin{qrcode, now_ms()};
    }
    json out;
    out["qrcode"] = qrcode;
    out["qr_content"] = content;
    out["qr_png_b64"] = qr_png_base64(content, 8, 3);
    out["expires_in"] = 300;
    return fish::core::Result<std::string>::ok(out.dump());
  } catch (const std::exception& e) {
    return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kUpstreamUnavailable,
                                                 std::string("parse:") + e.what());
  }
}

fish::core::Result<std::string> ControlService::wechat_qr_status(const std::string& qrcode) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (qrcode.empty() || wx_login_.qrcode != qrcode) {
      json out{{"status", "expired"}};
      return fish::core::Result<std::string>::ok(out.dump());
    }
    if (now_ms() - wx_login_.created_ms > kTaskTtlMs) {
      json out{{"status", "expired"}};
      return fish::core::Result<std::string>::ok(out.dump());
    }
  }

  // 这个端点会 hold 约 30 秒（长轮询），超时要给够
  auto r = fish::capability::HttpClient::instance().perform(
      {.method = "GET",
       .url = "https://ilinkai.weixin.qq.com/ilink/bot/get_qrcode_status?qrcode=" + qrcode,
       .headers = {{"AuthorizationType", "ilink_bot_token"}, {"X-WECHAT-UIN", "MQ=="}},
       .timeout_ms = 40000});
  if (!r.ok()) {
    json out{{"status", "wait"}, {"detail", r.error.detail}};
    return fish::core::Result<std::string>::ok(out.dump());
  }

  try {
    const auto j = json::parse(r.value->body);
    const std::string st = j.value("status", "");
    if (st == "wait" || st.empty()) {
      json out{{"status", "wait"}};
      return fish::core::Result<std::string>::ok(out.dump());
    }
    if (st == "confirmed") {
      const std::string token = j.value("bot_token", "");
      const std::string base = j.value("baseurl", j.value("bot_base_url", ""));
      if (token.empty()) {
        json out{{"status", "error"}, {"detail", "confirmed_without_token"}};
        return fish::core::Result<std::string>::ok(out.dump());
      }
      if (auto e = set_secret("channel.wechat", "bot_token", token); !e.ok()) {
        json out{{"status", "error"}, {"detail", "save_token:" + e.detail}};
        return fish::core::Result<std::string>::ok(out.dump());
      }
      {
        ConfigFile cf(cfg_.config_path);
        cf.load();
        cf.set("channel.wechat", "enabled", "true");
        if (!base.empty()) cf.set("channel.wechat", "bot_base_url", base);
        if (auto e = cf.save(); !e.ok()) {
          ::fish::log::log_line(::fish::log::LogLevel::kWarn, "control", "enable_wx_failed")
              .field("detail", e.detail);
        }
      }
      json out{{"status", "confirmed"}, {"bot_token_set", true}, {"saved", true}};
      if (!base.empty()) out["bot_base_url"] = base;
      return fish::core::Result<std::string>::ok(out.dump());
    }
    if (st == "expired") {
      json out{{"status", "expired"}};
      return fish::core::Result<std::string>::ok(out.dump());
    }
    json out{{"status", "wait"}};
    return fish::core::Result<std::string>::ok(out.dump());
  } catch (const std::exception& e) {
    json out{{"status", "error"}, {"detail", e.what()}};
    return fish::core::Result<std::string>::ok(out.dump());
  }
}

}  // namespace fish::control
