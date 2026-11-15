// 人格存储：全局默认 + 每会话覆盖，落盘为旁路 Markdown。
//
// 为什么单独成模块：
//   orchestrator 需要「按会话取人格」，而人格文本可能几百上千字。
//   塞进配置里换行转义难读，放目录里可以直接编辑、可以 git 管、随时能换。
module;

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>

export module fish.persona;

import fish.core;
import fish.log;

export namespace fish::persona {

namespace fs = std::filesystem;

// 两级人格 + 旁路文件：
//
//   全局默认：data/persona/default.md
//   会话覆盖：data/persona/sessions/<hash>.md
//
// 查找顺序：会话覆盖 → 全局默认 → 空。
// 空就是「没人设」，模型以裸状态说话，由用户自己填。
//
// 注意：persona_for() 在「该会话没有覆盖」时会回退到全局默认（通常非空），
// 所以调用方**不能**用它的返回值判空来区分「有没有会话级覆盖」——
// 那会让「无覆盖 → 退到 bot 级」的分支永远走不到。
// 判断有没有覆盖请用 has_override()。
class PersonaStore {
 public:
  struct Config {
    std::string dir{"data/persona"};
  };

  explicit PersonaStore(Config cfg);

  // 建目录。幂等。
  fish::core::Error init();

  // 全局默认人格。文件不存在或为空 → 返回空串。
  fish::core::Result<std::string> default_persona() const;

  // 写全局默认人格。空串等于删除（回到「无人格」）。
  fish::core::Error set_default_persona(const std::string& text);

  // 某会话的人格。没有覆盖时返回全局默认。
  // 想区分「有覆盖」和「回退到默认」，用 has_override()。
  fish::core::Result<std::string> persona_for(const std::string& conv_id) const;

  // 设某会话的人格覆盖。空串 = 取消覆盖。
  fish::core::Error set_persona_for(const std::string& conv_id, const std::string& text);

  // 该会话是否有单独的人覆盖
  [[nodiscard]] bool has_override(const std::string& conv_id) const;

  fish::core::Error clear_overrides();

  [[nodiscard]] const std::string& dir() const noexcept { return cfg_.dir; }

 private:
  [[nodiscard]] std::string default_path() const;
  [[nodiscard]] std::string override_path(const std::string& conv_id) const;
  // 读路径：新名优先，旧名（历史哈希）兜底，找到旧名顺手迁移。
  [[nodiscard]] std::string override_path_read(const std::string& conv_id) const;

  Config cfg_;
};

// 读文本文件：不存在返回空串（不算错误），只有读失败才报错。
fish::core::Result<std::string> read_text_file(const std::string& path);

// 写文本文件：自动建父目录，先写 .tmp 再改名。
fish::core::Error write_text_file(const std::string& path, const std::string& text);

}  // namespace fish::persona

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::persona {

namespace {

// 会话 id → 安全文件名。
// conv_id 可能是 "g:123456" 这种带冒号的，也可能是 QQ openid 长串，
// 直接当文件名跨平台不安全。用 FNV-1a 哈希，稳定且无依赖。
//
// 历史：这里原来用的是**非标准**偏移基 1469598103934665603（19 位，少一位），
// 改成标准值 14695981039346656037 之后，已经存在的
// data/persona/sessions/*.md 会全部命中不到——用户存过的人格会静默
// 回退到全局默认，界面上看不出异常。
//
// 所以现在的做法是「新名优先、旧名兜底」：
//   - 写：永远写标准哈希的新名
//   - 读：先找新名，没有就找旧名；找到旧名的顺手迁移成新名
// 迁移是幂等的：改名失败（权限等）不影响读取，下次启动再试。
//
// 同样的偏移基也用在 src/fish.media.cppm（内容寻址去重）和
// src/fish.memory.cppm（内容哈希）。那两处是**内容**哈希：
// 改了就换命名空间，媒体缓存要全量重算、索引要全量重建，
// 而且没有「读时兜底」这回事——所以只改 persona 这一处。
std::uint64_t fnv1a(const std::string& s, std::uint64_t basis) {
  std::uint64_t h = basis;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

std::string hash_hex(std::uint64_t h) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return std::string(buf);
}

// 标准 FNV-1a（新文件名）
std::string safe_filename(const std::string& conv_id) {
  return hash_hex(fnv1a(conv_id, 14695981039346656037ULL));
}

// 旧的非标准偏移基（历史文件名）。只用于读时兜底与迁移。
std::string legacy_filename(const std::string& conv_id) {
  return hash_hex(fnv1a(conv_id, 1469598103934665603ULL));
}

}  // namespace

PersonaStore::PersonaStore(Config cfg) : cfg_(std::move(cfg)) {}

std::string PersonaStore::default_path() const { return cfg_.dir + "/default.md"; }

std::string PersonaStore::override_path(const std::string& conv_id) const {
  return cfg_.dir + "/sessions/" + safe_filename(conv_id) + ".md";
}

// 读路径：新名优先，旧名兜底。
//
// 为什么要兜底：改了哈希基之后，历史人格文件还在旧名下。
// 不兜底就读不到 → 用户存过的人格静默回退到全局默认，界面上完全看不出。
// 只有读需要兜底；写永远写新名（见 set_persona_for）。
std::string PersonaStore::override_path_read(const std::string& conv_id) const {
  const std::string dir = cfg_.dir + "/sessions/";
  const std::string fresh = dir + safe_filename(conv_id) + ".md";
  std::error_code ec;
  if (fs::exists(fresh, ec)) return fresh;

  const std::string legacy = dir + legacy_filename(conv_id) + ".md";
  if (fs::exists(legacy, ec)) {
    // 顺手迁移：改名成功之后就走新名，下次不再走这条分支。
    // 迁移失败不影响读取（旧文件还在）。
    std::error_code mv_ec;
    fs::rename(legacy, fresh, mv_ec);
    if (!mv_ec) {
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "persona",
                            "legacy_filename_migrated")
          .field("conv", conv_id)
          .field("to", safe_filename(conv_id));
      return fresh;
    }
    return legacy;
  }
  return fresh;   // 两边都没有：返回新名（读出来是空串，写也写新名）
}

fish::core::Error PersonaStore::init() {
  std::error_code ec;
  fs::create_directories(cfg_.dir + "/sessions", ec);
  if (ec) {
    return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                   "mkdir_persona:" + ec.message());
  }
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "persona", "ready")
      .field("dir", cfg_.dir);
  return fish::core::Error::none();
}

fish::core::Result<std::string> PersonaStore::default_persona() const {
  return read_text_file(default_path());
}

fish::core::Error PersonaStore::set_default_persona(const std::string& text) {
  if (text.empty()) {
    std::error_code ec;
    fs::remove(default_path(), ec);
    return fish::core::Error::none();
  }
  return write_text_file(default_path(), text);
}

fish::core::Result<std::string> PersonaStore::persona_for(
    const std::string& conv_id) const {
  auto ov = read_text_file(override_path_read(conv_id));
  if (!ov.ok()) return ov;
  if (!ov.value->empty()) return ov;
  return default_persona();
}

fish::core::Error PersonaStore::set_persona_for(const std::string& conv_id,
                                                const std::string& text) {
  const std::string path = override_path(conv_id);
  if (text.empty()) {
    std::error_code ec;
    fs::remove(path, ec);
    // 取消覆盖时连旧名一起删，否则读时兜底会把它又捡回来。
    fs::remove(cfg_.dir + "/sessions/" + legacy_filename(conv_id) + ".md", ec);
    return fish::core::Error::none();
  }
  return write_text_file(path, text);
}

bool PersonaStore::has_override(const std::string& conv_id) const {
  std::error_code ec;
  const auto p = override_path_read(conv_id);
  return fs::exists(p, ec) && fs::file_size(p, ec) > 0;
}

fish::core::Error PersonaStore::clear_overrides() {
  std::error_code ec;
  fs::remove_all(cfg_.dir + "/sessions", ec);
  fs::create_directories(cfg_.dir + "/sessions", ec);
  return fish::core::Error::none();
}

// ── 文件读写 ────────────────────────────────────────────

fish::core::Result<std::string> read_text_file(const std::string& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return fish::core::Result<std::string>::ok(std::string{});
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fish::core::Result<std::string>::fail(fish::core::ErrorCode::kInternal,
                                                 "open_failed:" + path);
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return fish::core::Result<std::string>::ok(ss.str());
}

fish::core::Error write_text_file(const std::string& path, const std::string& text) {
  std::error_code ec;
  const auto parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);

  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                     "write_failed:" + path);
    }
    out << text;
    if (!out) {
      return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                     "write_failed:" + tmp);
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    return fish::core::Error::make(fish::core::ErrorCode::kInternal,
                                   "rename_failed:" + ec.message());
  }
  return fish::core::Error::none();
}

}  // namespace fish::persona
