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
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
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

// WorkBuddy OAuth 登录会话。
//
// state 与登录进度只放内存：进程重启即作废，这与上游语义一致
// （授权链接本身就是一次性的）。凭据落盘到 data/workbuddy.json。
struct WorkBuddyLogin {
  std::string login_id;
  std::string state;
  std::string auth_url;
  std::int64_t expires_ms{0};
  bool done{false};
  std::string error;
  // 登录成功后的账号信息（只用于展示）
  std::string nickname;
  std::string uid;
};

// WorkBuddy 凭据查询结果（给桥接层跨平台拉取用）。
//
// 为什么需要它：控制面跑在 WSL、桥接层跑在 Windows 宿主时，两边看到的
// 不是同一个文件系统路径（`\\wsl$\...` 与 `C:\...` 是两个不同的文件），
// --cred-file 永远对不上，GUI 扫码登录对桥就是零效果。改走 HTTP 拉取
// 与路径无关。
struct WorkBuddyCredentialResult {
  bool found{false};
  // found=true 时是凭据 JSON（access_token / refresh_token / expires_at /
  // domain / nickname / uid），与落盘文件同构。
  std::string body;
  // found=false 时说明原因：no_credential = 还没登录 / 文件不存在。
  std::string detail;
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

  // ── WorkBuddy 链接登录（OAuth）──
  //
  // 不需要填 Key：向桥接层申请一个 state，把授权链接交给 GUI 打开，
  // 用户在浏览器里登录后，桥接层轮询拿到凭据，我们只负责查结果。
  //
  // 三步与 dsh-connect-workbuddy 一致：
  //   POST /v2/plugin/auth/state?platform=workbuddy  → state + authUrl
  //   GET  /v2/plugin/auth/token?state=...           → accessToken + 账号资料
  //   GET  /v2/plugin/login/account                  → 昵称/uid（仅展示）
  fish::core::Result<std::string> workbuddy_login_start();
  fish::core::Result<std::string> workbuddy_login_status(const std::string& login_id);
  // 用已保存的凭据探一次桥接层，确认真的能出 DeepSeek。
  fish::core::Result<std::string> workbuddy_test();

  // ── WorkBuddy 凭据查询（跨平台桥接层用）──
  //
  // 读凭据文件并原样返回。文件不存在 → found=false（HTTP 404 + no_credential）。
  //
  // 凭据落盘路径可配：capability.workbuddy.cred_file（空 = data_dir 下的
  // workbuddy.json）。WSL 部署时把它指到 Windows 侧可见的路径即可，
  // 不必再让桥接层去猜路径。
  WorkBuddyCredentialResult workbuddy_credential();

  // 桥接层专用的独立令牌：只对凭据端点有效，不给主 token 用。
  //
  // 为什么要有独立令牌：这个端点返回明文 access_token。主 token 是给
  // GUI 用的（GUI 可能把配置展示出来、甚至截图分享），拿它换明文凭据
  // 风险太大。独立令牌只写在本机文件里、只认这一个端点，泄露了也只
  // 泄露 WorkBuddy 凭据，不会连带暴露控制面的其他能力。
  //
  // 优先读 config.toml 的 capability.workbuddy.credential_token；
  // 没配就用 data/control_credential_token.txt 里的（首次访问时自动生成）。
  [[nodiscard]] std::string workbuddy_credential_token();

 private:
  // 一次调用内复用的解析上下文：config.toml / .env.local 各只读一次，
  // resolved() 的结果按 "section.key" 记忆。
  //
  // 为什么需要：config_json() 会走 130+ 次 resolved()（每个字段一次，
  // 每个能力的 key_ready 又各自遍历 6 个 section），原来每次都重新
  // load 一遍文件，等于一次 GET /v1/config 上百次文件打开。
  //
  // 只在持 mu_ 的调用里使用，不跨调用共享，所以不需要额外同步；
  // 也因为是「一次调用即弃」，写操作（set_secret / apply_config_json）
  // 之后天然不会有陈旧缓存。
  struct ResolveCache {
    ResolveCache(ConfigFile& c, EnvFile& e) : cf(c), ef(e) {}
    ConfigFile& cf;
    EnvFile& ef;
    bool env_loaded{false};
    std::map<std::string, std::string> memo;
  };

  // 把密钥写进 .env.local，并把 config.toml 里的值保持为 ${VAR}
  fish::core::Error set_secret(const std::string& section, const std::string& key,
                               const std::string& value);

  // 同上，但改动用调用方已加载的 cf 承载（由调用方统一 save）。
  // apply_config_json 里必须用这个版本：否则 set_secret 自己 save 一次、
  // 调用方再 save 一次自己的旧快照，后写的把前一次改动覆盖掉。
  fish::core::Error set_secret(ConfigFile& cf, const std::string& section,
                               const std::string& key, const std::string& value,
                               bool* cf_changed);

  // 读当前生效的密钥值（config.toml 的 ${VAR} → .env.local → 进程环境）
  [[nodiscard]] std::string resolved(const std::string& section, const std::string& key);
  [[nodiscard]] std::string resolved(ResolveCache& rc, const std::string& section,
                                     const std::string& key);

  // 共享密钥：同一提供方在任一能力填过 key，其他能力选它时复用。
  // section 是「要用 key 的那个能力」，provider 是该能力当前选的提供方。
  [[nodiscard]] std::string shared_api_key(const std::string& section,
                                           const std::string& provider);
  [[nodiscard]] std::string shared_api_key(ResolveCache& rc, const std::string& section,
                                           const std::string& provider);

  // 凭据落盘路径。capability.workbuddy.cred_file 配了就用它（相对路径
  // 按 config.toml 所在目录解析），没配就是 data_dir/workbuddy.json。
  [[nodiscard]] std::string workbuddy_cred_path();

  Config cfg_;
  std::mutex mu_;
  QqBindTask qq_task_;
  WeChatLogin wx_login_;
  std::map<std::string, WorkBuddyLogin> wb_logins_;
  // 凭据专用令牌缓存：第一次用时生成并落盘，之后一直用同一个。
  // 不缓存的话每次鉴权都重读文件，桥接层就拿不到稳定的令牌了。
  std::string wb_credential_token_;
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

ControlService::ControlService(Config cfg) : cfg_(std::move(cfg)) {
  // data_dir 默认是相对路径 "data"，会随进程 cwd 漂移：从 systemd 拉起时
  // cwd 是 /，从仓库目录手跑时是仓库根，workbuddy.json / probe 缓存就会
  // 落到两个完全不同的地方，重启后「登录状态消失」这类怪事都出在这。
  //
  // 这里统一按「config.toml 所在目录」解析相对路径，与 config.toml /
  // .env.local / data/conversations.json 的定位口径一致；调用方显式传
  // 绝对路径时不动它。
  if (!cfg_.data_dir.empty()) {
    const fs::path p(cfg_.data_dir);
    if (p.is_relative()) {
      const fs::path base = fs::path(cfg_.config_path).parent_path();
      // config_path 只是 "config.toml"（没有目录）时 parent_path() 为空，
      // 那就等价于 cwd，用 absolute() 固定下来，别再留一个相对路径。
      std::error_code ec;
      const fs::path joined = base.empty() ? p : (base / p);
      const fs::path abs = fs::absolute(joined, ec);
      cfg_.data_dir = (ec ? joined : abs).lexically_normal().string();
    }
  }
}

std::string ControlService::resolved(const std::string& section, const std::string& key) {
  ConfigFile cf(cfg_.config_path);
  cf.load();
  EnvFile ef(cfg_.env_path);
  ResolveCache rc(cf, ef);
  return resolved(rc, section, key);
}

// 带缓存版本：config.toml / .env.local 由调用方加载一次，结果按
// "section.key" 记忆。config_json() 里每个字段都会调一次，缓存后
// 一次 GET /v1/config 只开两个文件。
std::string ControlService::resolved(ResolveCache& rc, const std::string& section,
                                     const std::string& key) {
  const std::string memo_key = section + '\x1f' + key;
  if (auto it = rc.memo.find(memo_key); it != rc.memo.end()) return it->second;

  std::string out;
  // 1) config.toml 里是 ${VAR} 占位符 → 去 .env.local / 环境变量找
  if (auto var = rc.cf.env_var_of(section, key)) {
    if (!rc.env_loaded) {
      rc.ef.load();
      rc.env_loaded = true;
    }
    if (auto v = rc.ef.get(*var)) {
      out = *v;
    } else if (const char* e = std::getenv(var->c_str()); e != nullptr) {
      out = e;
    }
  } else {
    // 2) 直接写字面值 → 用它
    out = rc.cf.get_or(section, key, "");
  }
  rc.memo.emplace(memo_key, out);
  return out;
}

fish::core::Error ControlService::set_secret(const std::string& section,
                                              const std::string& key,
                                              const std::string& value) {
  ConfigFile cf(cfg_.config_path);
  if (auto e = cf.load(); !e.ok()) return e;

  bool cf_changed = false;
  if (auto e = set_secret(cf, section, key, value, &cf_changed); !e.ok()) return e;
  if (cf_changed) return cf.save();
  return fish::core::Error::none();
}

// 密钥落盘的核心逻辑：变量名进 .env.local，config.toml 里只留 ${VAR}。
// config.toml 的改动交给调用方 save（cf_changed 告知是否改了），
// 这样 apply_config_json 能把自己的一批改动合并成一次写盘。
fish::core::Error ControlService::set_secret(ConfigFile& cf, const std::string& section,
                                              const std::string& key,
                                              const std::string& value,
                                              bool* cf_changed) {
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
    if (cf_changed != nullptr) *cf_changed = true;
  }

  EnvFile ef(cfg_.env_path);
  if (auto e = ef.load(); !e.ok()) return e;
  ef.set(var, value);
  return ef.save();
}

// ── 共享密钥 ────────────────────────────────────────────
//
// 同一提供方在任一能力里填过密钥，其他能力选它时直接复用：
//   capability.chat.api_key / capability.search.api_key /
//   capability.document.api_key / capability.embedding.api_key /
//   capability.stt.api_key / capability.tts.api_key
//
// 解析顺序：自己的 → 同提供方其他能力的 → DEEPSEEK_API_KEY 环境变量。
// 典型场景：DeepSeek 的 key 在对话里填过，搜索/文档选 DeepSeek 官方
// 时就不用再填一遍。
std::string ControlService::shared_api_key(const std::string& section,
                                           const std::string& provider) {
  ConfigFile cf(cfg_.config_path);
  cf.load();
  EnvFile ef(cfg_.env_path);
  ResolveCache rc(cf, ef);
  return shared_api_key(rc, section, provider);
}

std::string ControlService::shared_api_key(ResolveCache& rc, const std::string& section,
                                           const std::string& provider) {
  if (provider.empty() || provider == "workbuddy") return {};  // 桥接不校验
  if (std::string mine = resolved(rc, section, "api_key"); !mine.empty()) return mine;

  static constexpr const char* kKeySections[] = {
      "capability.chat",    "capability.search",
      "capability.document", "capability.embedding",
      "capability.stt",     "capability.tts",
  };
  for (const char* sec : kKeySections) {
    if (section == sec) continue;
    if (std::string p = resolved(rc, std::string(sec), "provider"); p == provider) {
      if (std::string k = resolved(rc, std::string(sec), "api_key"); !k.empty()) {
        return k;
      }
    }
  }
  // 兜底：DEEPSEEK_API_KEY 是官方唯一专用变量，很多用户只放这一把
  if (provider == "deepseek") {
    if (!rc.env_loaded) {
      rc.ef.load();
      rc.env_loaded = true;
    }
    if (auto v = rc.ef.get("DEEPSEEK_API_KEY")) return *v;
    if (const char* e = std::getenv("DEEPSEEK_API_KEY"); e != nullptr) return e;
  }
  return {};
}

// ── 配置视图 ────────────────────────────────────────────
//
// 每项：(section, key, 类型)。类型决定 JSON 里的形态。
// 密钥项的 key 后缀自动是 "_set"，值是「是否非空」。
namespace {

enum class FieldKind { kString, kInt, kBool, kSecret, kLocked };

struct FieldDef {
  const char* section;
  const char* key;
  FieldKind kind;
};

// 提供方预设。GUI 只选提供方，base_url / model 由这里定，用户不填。
//
// 只列「真能出 DeepSeek」的两家：底层模型锁死 deepseek-flash，
// 列一家不托管 DeepSeek 的厂商，用户选了也只会拿到别的模型，
// 等于绕开了锁定。
struct ProviderPreset {
  const char* provider;     // GUI 下拉里的选项值
  const char* base_url;
  const char* model;
  const char* api_key_hint; // GUI 提示文案
};

constexpr ProviderPreset kChatPresets[] = {
    // WorkBuddy：本机桥接层，走扫码/链接登录，key 桥不校验。
    // deep-model 实测路由到 deepseek-v4.1-flash（支持读图，上下文 1M）。
    {"workbuddy", "http://127.0.0.1:18091/v1", "deep-model", "点击下方按钮登录"},
    // DeepSeek 官方：直接填 API Key
    {"deepseek", "https://api.deepseek.com", "deepseek-flash", "sk-开头的 DeepSeek API Key"},
};

// 接入方式统一是 OpenAI 兼容协议；provider 字段只决定 dialect，
// 对 OpenAI 兼容端点一律写 "openai"。
constexpr const char* kChatDialect = "openai";

const ProviderPreset* find_chat_preset(const std::string& provider) {
  for (const auto& p : kChatPresets) {
    if (provider == p.provider) return &p;
  }
  return nullptr;
}

// 非 chat 能力的「provider → base_url」默认表。
//
// 两处共用同一份数据，必须只有一份定义：
//   1) config_json() 下发 capability.defaults，GUI 据此在切提供方时自动填；
//   2) apply_config_json() 在 provider 变化时据此改写 base_url。
// 原来只在 config_json() 里内联构造，后端切提供方时 base_url 停在旧值，
// 用户选了「阿里云百炼」实际连的还是硅基流动。
//
// 注意 chat 不在这里：chat 的预设还带 model / api_key_hint，
// 用上面的 kChatPresets。
struct ProviderDefault {
  const char* capability;   // embedding / stt / tts / search / document
  const char* provider;
  const char* base_url;
};

constexpr ProviderDefault kProviderDefaults[] = {
    // 向量
    {"embedding", "siliconflow", "https://api.siliconflow.cn/v1"},
    {"embedding", "ark", "https://ark.cn-beijing.volces.com/api/v3"},
    {"embedding", "dashscope", "https://dashscope.aliyuncs.com/compatible-mode/v1"},
    {"embedding", "qianfan", "https://qianfan.baidubce.com/v2"},
    // 语音识别
    {"stt", "siliconflow", "https://api.siliconflow.cn/v1"},
    {"stt", "volcengine", "https://openspeech.bytedance.com"},
    {"stt", "dashscope", "https://dashscope.aliyuncs.com"},
    {"stt", "baidu", "https://vop.baidu.com"},
    // 语音合成
    {"tts", "volcengine", "https://openspeech.bytedance.com"},
    {"tts", "dashscope", "https://dashscope.aliyuncs.com"},
    {"tts", "siliconflow", "https://api.siliconflow.cn/v1"},
    // 搜索 / 文档
    {"search", "deepseek", "https://api.deepseek.com"},
    {"document", "deepseek", "https://api.deepseek.com"},
};

// 该能力下 provider 的默认 base_url；没有预设返回 nullptr。
const char* find_provider_base_url(const std::string& capability,
                                   const std::string& provider) {
  for (const auto& d : kProviderDefaults) {
    if (capability == d.capability && provider == d.provider) return d.base_url;
  }
  return nullptr;
}

// 会被 GUI 当作「能力」暴露出来的五项（不含 chat，chat 单独走预设）。
constexpr const char* kPresetCapabilities[] = {"embedding", "stt", "tts", "search",
                                              "document"};

// 锁定字段的生效值。
//
// 目前只有 reasoning_effort 一个锁定字段，它的值**不来自 config.toml**：
// 出厂配置里根本没有这个键（运行时用的是 load_chat_config 的默认值），
// 原样返回就是空串。于是 /v1/config 回显 ""、GUI 显示空，而运行时
// 实际发给上游的是 "high" —— 显示层和真实行为对不上。
//
// 所以锁定字段一律回显锁定值本身，来源统一走
// fish::capability::locked_reasoning_effort()，与 PUT 拒绝时的
// effort_locked:<值>、运行时的兜底默认值同源。
std::string locked_field_value(const std::string& section, const std::string& key,
                                const ConfigFile& cf) {
  if (section == "capability.chat" && key == "reasoning_effort") {
    return fish::capability::locked_reasoning_effort();
  }
  return cf.get_or(section, key, "");
}

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
    {"capability.chat", "reasoning_effort", FieldKind::kLocked},
    {"capability.chat", "api_key", FieldKind::kSecret},
    // WorkBuddy 桥接（跨平台部署用）
    //   cred_file       凭据落盘路径；空 = data_dir/workbuddy.json
    //   credential_token 桥接层拉凭据用的独立令牌；空 = 自动生成到
    //                   data/control_credential_token.txt
    {"capability.workbuddy", "cred_file", FieldKind::kString},
    {"capability.workbuddy", "credential_token", FieldKind::kSecret},
    {"capability.embedding", "provider", FieldKind::kString},
    {"capability.embedding", "base_url", FieldKind::kString},
    {"capability.embedding", "model", FieldKind::kString},
    {"capability.embedding", "api_key", FieldKind::kSecret},
    {"capability.stt", "provider", FieldKind::kString},
    {"capability.stt", "base_url", FieldKind::kString},
    {"capability.stt", "model", FieldKind::kString},
    {"capability.stt", "language", FieldKind::kString},
    {"capability.stt", "path", FieldKind::kString},
    {"capability.stt", "need_wav16k", FieldKind::kBool},
    {"capability.stt", "api_key", FieldKind::kSecret},
    {"capability.tts", "provider", FieldKind::kString},
    {"capability.tts", "base_url", FieldKind::kString},
    {"capability.tts", "model", FieldKind::kString},
    {"capability.tts", "voice", FieldKind::kString},
    {"capability.tts", "format", FieldKind::kString},
    {"capability.tts", "app_id", FieldKind::kString},
    {"capability.tts", "cluster", FieldKind::kString},
    {"capability.tts", "api_key", FieldKind::kSecret},
    {"capability.tts", "group_voice", FieldKind::kBool},
    {"capability.document", "provider", FieldKind::kString},
    {"capability.document", "base_url", FieldKind::kString},
    {"capability.document", "api_key", FieldKind::kSecret},
    {"capability.search", "provider", FieldKind::kString},
    {"capability.search", "base_url", FieldKind::kString},
    {"capability.search", "api_key", FieldKind::kSecret},
    {"capability.search", "api_engine", FieldKind::kString},
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
  // 本次调用内共用的解析缓存：下面的 resolved() / shared_api_key()
  // 都走它，一次 GET /v1/config 只开 config.toml 和 .env.local 各一次
  // （原来是每个字段一次，130+ 次文件打开）。
  EnvFile ef(cfg_.env_path);
  ResolveCache rc(cf, ef);

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
          (*node)[f.key] = resolved(rc, f.section, f.key);
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
      case FieldKind::kSecret: {
        const bool own = !resolved(rc, f.section, f.key).empty();
        (*node)[std::string(f.key) + "_set"] = own;
        // key_ready：自己填过，或同一提供方在其他能力填过（共享密钥）。
        // GUI 用它决定还要不要让用户填 key。
        if (f.section == std::string_view("capability.embedding") ||
            f.section == std::string_view("capability.stt") ||
            f.section == std::string_view("capability.tts") ||
            f.section == std::string_view("capability.search") ||
            f.section == std::string_view("capability.document")) {
          const std::string sec(f.section);
          const std::string provider = resolved(rc, sec, "provider");
          (*node)["key_ready"] =
              own || !shared_api_key(rc, sec, provider).empty();
        }
        break;
      }
      case FieldKind::kLocked:
        // 锁定字段：回显锁定值（GUI 显示成只读），PUT 拒绝改。
        // 不能原样读 config.toml —— reasoning_effort 压根不在文件里，
        // 原样读就是空串，和运行时实际发出去的 "high" 对不上。
        (*node)[f.key] = locked_field_value(f.section, f.key, cf);
        break;
    }
  }

  // chat 的提供方预设：GUI 按 provider 下拉选择，下拉项和当前选中的
  // 提供方信息都从这里拿，前端不用硬编码表。
  {
    json& chat = root["capability"]["chat"];
    json items = json::array();
    for (const auto& p : kChatPresets) {
      items.push_back({{"provider", p.provider},
                       {"api_key_hint", p.api_key_hint}});
    }
    chat["presets"] = items;
    chat["model_locked"] = "deepseek-flash";
  }

  // 各能力的提供方清单：GUI 下拉直接用这个，前端不硬编码。
  // 加一家厂商只改 capability 模块，控制面自动暴露出去。
  {
    auto& cap = root["capability"];
    json providers = json::object();
    providers["embedding"] = {
        {"values", fish::capability::embedding_provider_list()},
        {"labels",
         json::object({{"siliconflow", "硅基流动"},
                       {"ark", "火山方舟"},
                       {"dashscope", "阿里云百炼"},
                       {"qianfan", "百度千帆"},
                       {"workbuddy", "WorkBuddy（本机桥接）"}})}};
    providers["stt"] = {
        {"values", fish::capability::stt_provider_list()},
        {"labels",
         json::object({{"siliconflow", "硅基流动（免费 SenseVoice）"},
                       {"volcengine", "火山引擎"},
                       {"dashscope", "阿里云百炼"},
                       {"deepseek", "DeepSeek 官方"}})}};
    providers["tts"] = {
        {"values", fish::capability::tts_provider_list()},
        {"labels",
         json::object({{"edge", "Edge TTS（免费，本机）"},
                       {"volcengine", "火山引擎"},
                       {"dashscope", "阿里云百炼 CosyVoice"},
                       {"siliconflow", "硅基流动 CosyVoice2"},
                       {"deepseek", "DeepSeek 官方"}})}};
    providers["search"] = {
        {"values", fish::capability::search_provider_list()},
        {"labels",
         json::object({{"none", "不启用"},
                       {"deepseek", "DeepSeek 官方"},
                       {"generic", "自定义端点"}})}};
    providers["document"] = {{"values", "deepseek,none"},
                             {"labels", json::object({{"deepseek", "DeepSeek Files API"},
                                                      {"none", "不启用"}})}};
    cap["providers"] = std::move(providers);
  }

  // 各能力里「换提供方时 base_url 也要跟着改」的默认值。
  // GUI 选了提供方后据此自动填，用户不必记各家的域名；
  // apply_config_json() 也读同一张 kProviderDefaults，两处不会跑偏。
  {
    auto& cap = root["capability"];
    json defaults = json::object();
    for (const char* name : kPresetCapabilities) defaults[name] = json::object();
    for (const auto& d : kProviderDefaults) {
      defaults[d.capability][d.provider] = d.base_url;
    }
    cap["defaults"] = std::move(defaults);
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
  // 解析缓存：下面「占位符键值没变就不覆盖」的判断会用到 resolved()，
  // 走缓存避免每个字符串字段都重开一次文件。写操作只碰 cf / ef，
  // 缓存随本函数结束丢弃，不存在陈旧问题。
  EnvFile ef(cfg_.env_path);
  ResolveCache rc(cf, ef);

  bool dirty = false;
  bool restart = false;

  // 摊平成 "channel.qq_official.enabled" → value，跟 kFields 的
  // "section.key" 对得上（section 本身可能带点）。
  std::vector<std::pair<std::string, json>> flat;
  flatten_json("", in, &flat);

  // 请求里显式给了 base_url 的字段：能力切提供方时不能覆盖用户的显式值。
  // 必须先扫一遍——摊平后的顺序不保证 provider 在 base_url 之前。
  std::set<std::string> explicit_base_urls;
  for (const auto& [full, val] : flat) {
    if (val.is_string() && full.size() > 9 &&
        full.compare(full.size() - 9, 9, ".base_url") == 0) {
      explicit_base_urls.insert(full);
    }
  }

  // 请求里显式带了 api_key 的能力：用户自己填了 key。
  std::set<std::string> sections_with_explicit_key;
  for (const auto& [full, val] : flat) {
    if (val.is_string() && full.size() > 8 &&
        full.compare(full.size() - 8, 8, ".api_key") == 0) {
      sections_with_explicit_key.insert(full.substr(0, full.size() - 8));
    }
  }

  // ── 切提供方 → 旧 key 解绑（前置处理）────────────────────
  //
  // 为什么必须清：config.toml 里 capability.stt.api_key 往往写的是
  // ${SILICONFLOW_API_KEY}，展开后是硅基流动那把真 key。从 siliconflow
  // 切到 deepseek 后若不动它，运行时 resolve_provider_key 第一步
  // 「自己的 api_key 非空」就命中 → 拿硅基流动的 key 去请求 DeepSeek
  // → 401。清空后落到共享密钥逻辑（同提供方其他能力借一把，
  // 或 DEEPSEEK_API_KEY 环境变量兜底）。
  //
  // 必须在主循环**之前**做，原因有两个：
  //   1) JSON 对象的键是按字典序摊平的（api_key < base_url < provider），
  //      同一个 section 里 api_key 会先于 provider 被处理。先解绑，
  //      set_secret 才会给新 key 建一个新的 ${VAR}，而不是把新 key
  //      写进旧提供方那个共享变量里（那会让 embedding 之类的其他能力
  //      也拿到这把新 key，直接串了）。
  //   2) 用户这次填了新 key 的话，先解绑再 set_secret，
  //      新旧两把 key 各归各位。
  //
  // 只在 provider 真的变了时才清：GUI 每次保存都会把 provider 带上，
  // 不比对就会把用户刚填的 key 每次保存都抹掉。
  //
  // .env.local 里的变量不动 —— 同一个 ${VAR} 可能被别的能力引用，
  // 而且切回原提供方时还能靠共享密钥逻辑借回来。
  //
  // 只对「运行时走 resolve_provider_key 的能力」清：
  //   能清：embedding / stt / tts / search —— 它们找不到自己的 key 时
  //         会去同提供方别的能力借一把，或用 DEEPSEEK_API_KEY 兜底。
  //   不能清：chat / document / image —— 它们的 key 是直读的
  //         （load_chat_config / load_config），一清空这个能力直接瘫掉：
  //         chat 空 key → make_chat 失败 → 退回 EchoChat 桩（只复读不报错），
  //         document 空 key → make_parser 返回 nullptr → 文档解析整个停掉。
  //         这两个宁可 401（用户看得见），也不能静默降级。
  static constexpr const char* kClearableKeySections[] = {
      "capability.embedding", "capability.stt", "capability.tts", "capability.search",
  };
  auto clearable_key_section = [](const std::string& sec) {
    for (const char* s : kClearableKeySections) {
      if (sec == s) return true;
    }
    return false;
  };
  for (const auto& [full, val] : flat) {
    if (!val.is_string()) continue;
    if (full.size() <= 9 || full.compare(full.size() - 9, 9, ".provider") != 0) continue;
    const std::string sec = full.substr(0, full.size() - 9);
    // chat 的 provider 在 config.toml 里存的是 dialect（"openai"），
    // 不是预设名，拿它跟预设名比永远不相等 → 会误判成「换提供方」。
    // chat 由下面的预设分支处理，而且它的 key 也不能清（见上）。
    if (sec == "capability.chat") continue;
    if (!clearable_key_section(sec)) continue;
    const std::string new_provider = val.get<std::string>();
    const std::string old_provider = cf.get_or(sec, "provider", "");
    if (old_provider == new_provider) continue;   // 没换提供方，不动 key
    // 只清「占位符绑定」（值形如 ${VAR}）：真 key 还在 .env.local 里，
    // 清掉只是解绑，随时能借回来，丢不了东西。
    //
    // 手写的明文 key（api_key = "sk-xxx"）不动：清掉就是永久丢失用户
    // 手抄的密钥，换来的是一个一样的 401（错的 key 一样 401）。这种
    // 情况只记一条日志，提示用户自己换。
    const auto var = cf.env_var_of(sec, "api_key");
    if (!var.has_value()) {
      if (!cf.get_or(sec, "api_key", "").empty()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "control",
                              "api_key_may_not_match_provider")
            .field("section", sec)
            .field("from", old_provider)
            .field("to", new_provider);
      }
      continue;
    }
    cf.set(sec, "api_key", "");
    rc.memo.clear();   // cf 变了，缓存作废（后面 resolved() 要用）
    changed->push_back(sec + ".api_key");
    dirty = true;
    restart = true;
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "control",
                          "api_key_unbound_on_provider_change")
        .field("section", sec)
        .field("from", old_provider)
        .field("to", new_provider)
        .field("user_supplied_new_key",
               sections_with_explicit_key.count(sec) > 0);
  }

  for (const auto& [full, val] : flat) {
    // 特例优先：chat.provider 发的是预设名（workbuddy / deepseek /
    // siliconflow），展开成 dialect + base_url + model。
    // 这个分支必须在 FieldDef 匹配之前——"deepseek" 同时也是合法的
    // dialect 值，先匹配到 FieldDef 就只会改 provider 一个字段，
    // base_url/model 不动，用户切了提供方实际连的还是老地址。
    if (full == "capability.chat.provider" && val.is_string()) {
      const std::string name = val.get<std::string>();
      if (const auto* preset = find_chat_preset(name)) {
        // 旧 base_url 要在覆盖之前取：它就是「原来用的是哪个提供方」的
        // 判据（config.toml 里存的是 dialect "openai"，不存预设名）。
        const std::string old_base = cf.get_or("capability.chat", "base_url", "");
        if (cf.get_or("capability.chat", "provider", "") != kChatDialect) {
          cf.set("capability.chat", "provider", kChatDialect);
          changed->push_back("capability.chat.provider");
          dirty = true;
          restart = true;
        }
        if (cf.get_or("capability.chat", "base_url", "") != preset->base_url) {
          cf.set("capability.chat", "base_url", preset->base_url);
          changed->push_back("capability.chat.base_url");
          dirty = true;
          restart = true;
        }
        const std::string want_model = preset->model;
        if (cf.get_or("capability.chat", "model", "") != want_model) {
          cf.set("capability.chat", "model", want_model);
          changed->push_back("capability.chat.model");
          dirty = true;
          restart = true;
        }
        // 换提供方了，旧 key 不再属于新提供方。
        //
        // 注意这里**不**清空 chat 的 api_key（其它能力会清）：
        //   - chat 的 key 在运行时是直读 capability.chat.api_key 的，
        //     没有共享密钥兜底（load_chat_config 不走 resolve_provider_key）；
        //   - 而 make_chat 要求 key 非空，空了会退回 EchoChat 桩 ——
        //     机器人从此只会复读，不报错，比 401 更难查。
        // 所以这里只记一条日志，由 GUI 提示用户重新填 Key。
        if (old_base != preset->base_url) {
          if (sections_with_explicit_key.count("capability.chat") == 0) {
            ::fish::log::log_line(::fish::log::LogLevel::kWarn, "control",
                                  "chat_key_may_not_match_provider")
                .field("old_base", old_base)
                .field("new_base", preset->base_url);
          }
        }
        continue;
      }
    }

    // 非 chat 能力切提供方：base_url 要跟着改，否则用户选了别家
    // 实际连的还是老地址。与上面 chat 的分支同理，必须在 FieldDef
    // 匹配之前处理，否则只改到 provider 一个字段。
    //
    // 用户显式传了 base_url 时以用户为准（explicit_base_urls）。
    if (val.is_string() && full.size() > 9 &&
        full.compare(full.size() - 9, 9, ".provider") == 0) {
      const std::string sec = full.substr(0, full.size() - 9);
      // chat 不走这个分支：它的 provider 在 config.toml 里存的是 dialect
      // （"openai"），不是预设名，拿它跟预设名比永远不相等 —— 会误判成
      // 「换提供方」。chat 由上面的预设分支处理。
      if (sec != "capability.chat") {
      const std::string base_key = sec + ".base_url";
      const std::string new_provider = val.get<std::string>();
      if (explicit_base_urls.find(base_key) == explicit_base_urls.end()) {
        const auto dot = sec.rfind('.');
        const std::string cap = dot == std::string::npos ? sec : sec.substr(dot + 1);
        if (const char* url = find_provider_base_url(cap, new_provider);
            url != nullptr && !std::string_view(url).empty()) {
          if (cf.get_or(sec, "base_url", "") != url) {
            cf.set(sec, "base_url", url);
            rc.memo.clear();   // cf 变了，缓存作废（下面还要用 resolved）
            changed->push_back(base_key);
            dirty = true;
            restart = true;
          }
        }
      }
      }
    }

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

    // 模型锁定：直接改 capability.chat.model 一律拒绝
    if (full == "capability.chat.model") {
      return fish::core::Error::make(fish::core::ErrorCode::kBadRequest,
                                     "model_locked:deepseek-flash");
    }
    // 思考强度锁死 high：产品决定，不允许切换。
    // 报错里的值和 GET 回显、运行时默认值同源（locked_reasoning_effort）。
    if (full == "capability.chat.reasoning_effort") {
      return fish::core::Error::make(fish::core::ErrorCode::kBadRequest,
                                     std::string("effort_locked:") +
                                         fish::capability::locked_reasoning_effort());
    }

    // 密钥三态：__keep__ 不动，空串清除，其余覆盖
    if (def->kind == FieldKind::kSecret) {
      if (!val.is_string()) {
        return fish::core::Error::make(fish::core::ErrorCode::kBadRequest,
                                       "secret_not_string:" + full);
      }
      const std::string v = val.get<std::string>();
      if (v == "__keep__") continue;
      // 用带 cf 的重载：config.toml 的改动留给本函数最后统一 save，
      // 否则 set_secret 先写一次、这里再写一次自己的旧快照，互相覆盖。
      bool cf_changed = false;
      if (auto e = set_secret(cf, def->section, def->key, v, &cf_changed); !e.ok()) {
        return e;
      }
      if (cf_changed) {
        dirty = true;
        rc.memo.clear();   // cf 变了，缓存作废
      }
      changed->push_back(full);
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
      if (resolved(rc, def->section, def->key) == text) continue;
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

// ── WorkBuddy 链接登录 ──────────────────────────────────
//
// 与 dsh-connect-workbuddy 的 oauth.ts 同一套流程：
//   1. POST {base}/v2/plugin/auth/state?platform=workbuddy
//      → { state, authUrl }
//   2. 用户在浏览器里打开 authUrl 完成登录
//   3. GET  {base}/v2/plugin/auth/token?state={state}
//      → code 为 0/200 且带 accessToken 即成
//
// 凭据落到 data/workbuddy.json，之后桥接层每次请求读它。

namespace {

constexpr const char* kWorkBuddyBase = "https://www.codebuddy.cn";
constexpr const char* kWorkBuddyGlobalBase = "https://www.workbuddy.ai";
constexpr const char* kWorkBuddyApiPrefix = "/v2/plugin";

// 上游字段名有 camelCase / snake_case 两种写法，都要认。
std::string first_str(const json& j, std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    if (j.contains(k) && j[k].is_string()) {
      const auto v = j[k].get<std::string>();
      if (!v.empty()) return v;
    }
  }
  return {};
}

// 响应可能是 {data: {...}} 包了一层，也可能就是扁平的
const json& envelope(const json& j) {
  if (j.is_object() && j.contains("data") && j["data"].is_object()) return j["data"];
  return j;
}

std::string random_id() {
  std::string raw(8, '\0');
  if (FILE* f = std::fopen("/dev/urandom", "rb")) {
    std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
  }
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (unsigned char c : raw) {
    out += hex[c >> 4];
    out += hex[c & 0x0F];
  }
  return out;
}

}  // namespace

fish::core::Result<std::string> ControlService::workbuddy_login_start() {
  json out;
  const std::string url = std::string(kWorkBuddyBase) + kWorkBuddyApiPrefix +
                          "/auth/state?platform=workbuddy";
  auto r = fish::capability::HttpClient::instance().post_json(
      url, {{"Content-Type", "application/json"}}, "{}", 30000);
  if (!r.ok()) {
    out["ok"] = false;
    out["error"] = "unreachable";
    out["detail"] = r.error.detail;
    return fish::core::Result<std::string>::ok(out.dump());
  }

  try {
    const auto j = json::parse(r.value->body);
    const auto data = envelope(j);
    const std::string state = first_str(data, {"state", "State"});
    if (state.empty()) {
      out["ok"] = false;
      out["error"] = "no_state";
      out["detail"] = r.value->body.substr(0, 200);
      return fish::core::Result<std::string>::ok(out.dump());
    }
    // 上游通常直接给 authUrl；没给就按约定自己拼
    std::string auth = first_str(data, {"authUrl", "auth_url", "url", "Url"});
    if (auth.empty()) {
      auth = std::string(kWorkBuddyBase) + "/login?state=" + state;
    }

    WorkBuddyLogin s;
    s.login_id = "wb_" + random_id();
    s.state = state;
    s.auth_url = auth;
    s.expires_ms = now_ms() + 600000;   // 10 分钟，与上游一致

    {
      std::lock_guard<std::mutex> lock(mu_);
      // 会话表上限 8：先淘汰所有已过期的（正常过期，不打扰任何人），
      // 还满就删最旧的一个。绝不能 clear()——用户连点几次「登录」会把
      // 正在扫码的会话一起废掉，GUI 那边全部收到 session_not_found。
      const std::int64_t now = now_ms();
      for (auto it = wb_logins_.begin(); it != wb_logins_.end();) {
        if (it->second.expires_ms > 0 && it->second.expires_ms < now) {
          it = wb_logins_.erase(it);
        } else {
          ++it;
        }
      }
      if (wb_logins_.size() >= 8) {
        auto oldest = wb_logins_.begin();
        for (auto it = wb_logins_.begin(); it != wb_logins_.end(); ++it) {
          if (it->second.expires_ms < oldest->second.expires_ms) oldest = it;
        }
        wb_logins_.erase(oldest);
      }
      wb_logins_[s.login_id] = s;
    }

    out["ok"] = true;
    out["login_id"] = s.login_id;
    out["auth_url"] = auth;
    out["expires_in"] = 600;
    // 同一个链接也渲染成二维码，手机扫比在电脑上点更顺
    out["qr_png_base64"] = qr_png_base64(auth);
  } catch (const std::exception& e) {
    out["ok"] = false;
    out["error"] = "parse_failed";
    out["detail"] = e.what();
  }
  return fish::core::Result<std::string>::ok(out.dump());
}

fish::core::Result<std::string> ControlService::workbuddy_login_status(
    const std::string& login_id) {
  json out;
  WorkBuddyLogin snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = wb_logins_.find(login_id);
    if (it == wb_logins_.end()) {
      out["ok"] = false;
      out["done"] = true;
      out["error"] = "session_not_found";
      out["detail"] = "登录会话不存在或已过期，请重新发起";
      return fish::core::Result<std::string>::ok(out.dump());
    }
    snapshot = it->second;
  }

  if (snapshot.done) {
    out["ok"] = true;
    out["done"] = true;
    if (!snapshot.error.empty()) {
      out["success"] = false;
      out["error"] = snapshot.error;
    } else {
      out["success"] = true;
      out["nickname"] = snapshot.nickname;
      out["uid"] = snapshot.uid;
    }
    return fish::core::Result<std::string>::ok(out.dump());
  }
  if (now_ms() > snapshot.expires_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = wb_logins_.find(login_id);
    if (it != wb_logins_.end()) {
      it->second.done = true;
      it->second.error = "timeout";
    }
    out["ok"] = true;
    out["done"] = true;
    out["success"] = false;
    out["error"] = "timeout";
    return fish::core::Result<std::string>::ok(out.dump());
  }

  // 轮询上游。用户还没登录时 code 不为 0/200，属于正常「未完成」。
  const std::string url = std::string(kWorkBuddyBase) + kWorkBuddyApiPrefix +
                          "/auth/token?state=" + snapshot.state;
  fish::core::Result<fish::capability::HttpResult> r =
      fish::core::Result<fish::capability::HttpResult>::fail(
          fish::core::ErrorCode::kInternal, "unset");
  {
    fish::capability::HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.headers = {{"Accept", "application/json"}};
    req.timeout_ms = 20000;
    req.max_retries = 0;
    r = fish::capability::HttpClient::instance().perform(req);
  }
  if (!r.ok()) {
    // 网络抖动不算结束，让前端继续轮询
    out["ok"] = true;
    out["done"] = false;
    return fish::core::Result<std::string>::ok(out.dump());
  }

  try {
    const auto j = json::parse(r.value->body);
    const int code = j.contains("code") && j["code"].is_number() ? j["code"].get<int>() : -1;
    const auto data = envelope(j);
    const std::string token = first_str(data, {"accessToken", "access_token", "AccessToken"});
    if ((code != 0 && code != 200) || token.empty()) {
      out["ok"] = true;
      out["done"] = false;   // 用户还没完成
      return fish::core::Result<std::string>::ok(out.dump());
    }

    // 拿账号资料，只为展示
    std::string nickname, uid;
    {
      std::map<std::string, std::string> h{{"Accept", "application/json"},
                                           {"Authorization", "Bearer " + token}};
      const std::string domain = first_str(data, {"domain", "Domain"});
      if (!domain.empty()) h["X-Domain"] = domain;
      fish::capability::HttpRequest req;
      req.method = "GET";
      req.url = std::string(kWorkBuddyBase) + kWorkBuddyApiPrefix + "/login/account";
      req.headers = h;
      req.timeout_ms = 20000;
      req.max_retries = 0;
      if (auto pr = fish::capability::HttpClient::instance().perform(req); pr.ok()) {
        try {
          const auto pj = envelope(json::parse(pr.value->body));
          nickname = first_str(pj, {"nickname", "NickName", "ScreenName"});
          uid = first_str(pj, {"uid", "Uid", "userId", "UserID"});
        } catch (const std::exception&) {
        }
      }
    }

    // 落盘：桥接层下次请求读它
    //
    // 路径可配：capability.workbuddy.cred_file 配了就写那儿（跨平台部署
    // 时控制面和桥接层看到的路径不同时，这是唯一能对上的办法）；
    // 没配就是 data_dir/workbuddy.json（保持原有行为）。
    //
    // data_dir 已在构造函数里解析成绝对路径，但目录本身可能还不存在
    // （首次部署、只带了 config.toml）。先建目录，否则 ofstream 静默失败，
    // 用户看到「登录成功」而凭据根本没写下去。
    const std::string path = workbuddy_cred_path();
    {
      std::error_code ec;
      const fs::path parent = fs::path(path).parent_path();
      if (!parent.empty()) fs::create_directories(parent, ec);
    }
    json file;
    file["version"] = 1;
    file["access_token"] = token;
    file["refresh_token"] = first_str(data, {"refreshToken", "refresh_token"});
    file["expires_at"] = data.value("expiresAt", std::int64_t{0});
    file["domain"] = first_str(data, {"domain", "Domain"});
    file["nickname"] = nickname;
    file["uid"] = uid;
    file["saved_at"] = now_ms();
    std::ofstream f(path, std::ios::binary);
    if (f) {
      f << file.dump(2);
      f.close();
    } else {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "control",
                            "workbuddy_save_failed")
          .field("path", path);
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = wb_logins_.find(login_id);
      if (it != wb_logins_.end()) {
        it->second.done = true;
        it->second.nickname = nickname;
        it->second.uid = uid;
      }
    }

    out["ok"] = true;
    out["done"] = true;
    out["success"] = true;
    out["nickname"] = nickname;
    out["uid"] = uid;
  } catch (const std::exception& e) {
    out["ok"] = true;
    out["done"] = false;
    out["detail"] = e.what();
  }
  return fish::core::Result<std::string>::ok(out.dump());
}

// ── WorkBuddy 凭据查询 ──────────────────────────────────

std::string ControlService::workbuddy_cred_path() {
  // 优先级：config.toml 的 capability.workbuddy.cred_file > data_dir/workbuddy.json
  //
  // 相对路径按 config.toml 所在目录解析（与 data_dir / .env.local 口径一致），
  // 否则 cwd 一变，凭据就写到别处去了。
  const std::string configured = resolved("capability.workbuddy", "cred_file");
  if (!configured.empty()) {
    const fs::path p(configured);
    if (p.is_absolute()) return p.lexically_normal().string();
    const fs::path base = fs::path(cfg_.config_path).parent_path();
    std::error_code ec;
    const fs::path joined = base.empty() ? p : (base / p);
    const fs::path abs = fs::absolute(joined, ec);
    return (ec ? joined : abs).lexically_normal().string();
  }
  return (fs::path(cfg_.data_dir) / "workbuddy.json").lexically_normal().string();
}

WorkBuddyCredentialResult ControlService::workbuddy_credential() {
  WorkBuddyCredentialResult out;
  const std::string path = workbuddy_cred_path();

  std::error_code ec;
  if (!fs::exists(path, ec) || fs::is_directory(path, ec)) {
    out.found = false;
    out.detail = "凭据文件不存在：" + path;
    return out;
  }

  std::ifstream f(path, std::ios::binary);
  if (!f) {
    out.found = false;
    out.detail = "凭据文件读不了：" + path;
    return out;
  }
  std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  f.close();

  // 原样透传给桥接层：桥接层已经认识这个格式（access_token /
  // refresh_token / expires_at / domain / nickname / uid），
  // 控制面不重复解析，避免两边对「expires_at 是秒还是毫秒」的理解漂移。
  //
  // 但要确认它确实是个带 access_token 的 JSON：把一份坏文件当凭据发给
  // 桥接层，只会让桥接层报一句莫名其妙的 401。
  try {
    const auto j = json::parse(body);
    if (!j.is_object() || !j.contains("access_token") ||
        !j["access_token"].is_string() || j["access_token"].get<std::string>().empty()) {
      out.found = false;
      out.detail = "凭据文件格式不对（缺 access_token）：" + path;
      return out;
    }
  } catch (const std::exception& e) {
    out.found = false;
    out.detail = std::string("凭据文件不是合法 JSON：") + e.what();
    return out;
  }

  out.found = true;
  out.body = std::move(body);
  return out;
}

std::string ControlService::workbuddy_credential_token() {
  // 1) 配置里显式给了就用它（用户想跨机用同一个令牌时可以自己填）
  const std::string configured = resolved("capability.workbuddy", "credential_token");
  if (!configured.empty()) return configured;

  // 2) 否则用 data/control_credential_token.txt 里的，没有就生成一个
  //
  // 为什么放文件而不是放 config.toml：它是一个凭据，config.toml 是
  // 「非密钥配置」且经常被贴到 issue 里。放 data/ 下（.gitignore 已忽略）
  // 并设 600 权限，和 .env.local 同一套口径。
  const fs::path path = fs::path(cfg_.data_dir) / "control_credential_token.txt";
  std::error_code ec;
  if (fs::exists(path, ec)) {
    std::ifstream f(path);
    std::string tok;
    std::getline(f, tok);
    tok = trim(tok);
    if (!tok.empty()) {
      std::lock_guard<std::mutex> lock(mu_);
      wb_credential_token_ = tok;
      return tok;
    }
  }

  // 生成：32 字节随机 → hex
  std::string raw(32, '\0');
  if (FILE* rf = std::fopen("/dev/urandom", "rb")) {
    const auto n = std::fread(raw.data(), 1, raw.size(), rf);
    std::fclose(rf);
    if (n != raw.size()) raw.resize(n);
  }
  if (raw.size() != 32) {
    std::uint64_t a = static_cast<std::uint64_t>(now_ms());
    for (std::size_t i = 0; i < raw.size(); ++i) {
      raw[i] = static_cast<char>((a >> ((i % 8) * 8)) ^ i);
    }
  }
  static const char* hex = "0123456789abcdef";
  std::string tok;
  tok.reserve(raw.size() * 2);
  for (unsigned char c : raw) {
    tok += hex[c >> 4];
    tok += hex[c & 0x0F];
  }

  fs::create_directories(cfg_.data_dir, ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (f) {
    f << tok << "\n";
    f.close();
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
  } else {
    // 写不下去就退回进程内临时值：本次运行仍然能用（桥接层重启后
    // 需要重新读文件，那时用户会看到明确报错）。
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "control",
                          "credential_token_write_failed")
        .field("path", path.string());
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    wb_credential_token_ = tok;
  }
  return tok;
}

fish::core::Result<std::string> ControlService::workbuddy_test() {
  // 桥接层是本机的 OpenAI 兼容端点：问一次 /v1/models 就知道它活着没有，
  // 顺便确认模型名还在目录里。不消耗任何额度。
  json out;
  const std::string base = resolved("capability.chat", "base_url");
  const std::string url =
      (base.empty() ? std::string("http://127.0.0.1:18091/v1") : base) + "/models";

  fish::capability::HttpRequest req;
  req.method = "GET";
  req.url = url;
  req.headers = {{"Accept", "application/json"}};
  req.timeout_ms = 8000;
  req.max_retries = 0;
  auto r = fish::capability::HttpClient::instance().perform(req);
  if (!r.ok()) {
    out["ok"] = false;
    out["error"] = "bridge_unreachable";
    out["detail"] = "桥接层没响应（" + url + "），先启动 WorkBuddy 桥接";
    return fish::core::Result<std::string>::ok(out.dump());
  }
  try {
    const auto j = json::parse(r.value->body);
    json ids = json::array();
    for (const auto& m : j.value("data", json::array())) {
      if (m.is_object() && m.contains("id")) ids.push_back(m["id"]);
    }
    out["ok"] = true;
    out["models"] = ids;
    // 登录状态：问桥的 /v1/whoami（只读诊断端点，回答「桥现在实际拿谁在发请求」，
    // 可能还会热重载 cred-file），而不是看 data/workbuddy.json 文件存在与否。
    fish::capability::HttpRequest who;
    who.method = "GET";
    who.url = (base.empty() ? std::string("http://127.0.0.1:18091/v1") : base) + "/whoami";
    who.headers = {{"Accept", "application/json"}};
    who.timeout_ms = 8000;
    who.max_retries = 0;
    auto w = fish::capability::HttpClient::instance().perform(who);
    if (w.ok()) {
      try {
        const auto wj = json::parse(w.value->body);
        for (const char* k : {"region", "domain", "nickname", "source", "expires_in"}) {
          if (wj.contains(k)) out[k] = wj[k];
        }
        out["logged_in"] = wj.value("logged_in", false);
      } catch (const std::exception&) {
        out["logged_in"] = false;
        out["source"] = "none";
      }
    } else {
      out["logged_in"] = false;
      out["source"] = "none";
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

      // 扫码成功即落盘：用户扫完就该能用，不该再让他手抄两串密钥。
      //
      // 整段（config.toml + .env.local）必须在 mu_ 里：apply_config_json
      // 也持 mu_ 写同一份文件，两处并发会各自基于旧快照 save，后写的
      // 把前一次改动整段覆盖掉。本函数入口只在读 qq_task_ 时短暂持锁，
      // 这里重新加锁不会自锁。
      {
        std::lock_guard<std::mutex> lock(mu_);
        ConfigFile cf(cfg_.config_path);
        if (auto e = cf.load(); !e.ok()) {
          json out{{"status", "error"}, {"detail", "load_config:" + e.detail}};
          return fish::core::Result<std::string>::ok(out.dump());
        }
        bool cf_changed = false;
        if (auto e = set_secret(cf, "channel.qq_official", "app_id", app_id,
                                &cf_changed);
            !e.ok()) {
          json out{{"status", "error"}, {"detail", "save_app_id:" + e.detail}};
          return fish::core::Result<std::string>::ok(out.dump());
        }
        if (auto e = set_secret(cf, "channel.qq_official", "app_secret",
                                *secret.value, &cf_changed);
            !e.ok()) {
          json out{{"status", "error"}, {"detail", "save_secret:" + e.detail}};
          return fish::core::Result<std::string>::ok(out.dump());
        }
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
      // 同 qq_qr_status：整段落盘持 mu_，避免与 apply_config_json 并发
      // 各自基于旧快照 save 互相覆盖。
      {
        std::lock_guard<std::mutex> lock(mu_);
        ConfigFile cf(cfg_.config_path);
        if (auto e = cf.load(); !e.ok()) {
          json out{{"status", "error"}, {"detail", "load_config:" + e.detail}};
          return fish::core::Result<std::string>::ok(out.dump());
        }
        bool cf_changed = false;
        if (auto e = set_secret(cf, "channel.wechat", "bot_token", token, &cf_changed);
            !e.ok()) {
          json out{{"status", "error"}, {"detail", "save_token:" + e.detail}};
          return fish::core::Result<std::string>::ok(out.dump());
        }
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
