// src/fish.doc.cppm
// 文档输入：把 PDF / Word / PPT / 表格 / 纯文本变成模型能读的上下文。
//
// 为什么单独一层：
//   文档解析和「模型能力」不是一回事——它有自己的失败模式
//   （页数超限、加密、格式不支持、解析超时）、自己的成本模型
//   （按页计费）、自己的生命周期（上传一次，多轮引用）。塞进
//   capability 会让那个模块既管协议差异又管文件解析。
//
// 两条通道：
//   parse()  —— 一次性把内容取成文本，直接拼进本轮上下文。
//               适合小文档、不需要跨轮保留的场景。
//   upload() —— 交给厂商做服务端解析，拿回一个 file_id，
//               之后每轮只传 id，内容由厂商侧保留。
//               适合大文档、需要多轮引用的场景。
module;
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

export module fish.doc;

import fish.core;
import fish.log;
import fish.config;
import fish.capability;

export namespace fish::doc {

// 解析方式：决定模型能不能看到文档里的图。
//   text   —— 只取文字。文字为主的文档（合同、论文）用它，便宜。
//   layout —— 文字 + 图。带图表的报告、幻灯片用它，按图计费。
enum class Mode : std::uint8_t { kText, kLayout };

struct Config {
  std::string provider{"none"};   // none | deepseek | openai
  std::string base_url{"https://api.deepseek.com"};
  std::string api_key;
  int timeout_ms{180000};         // 解析慢，给足时间
  std::size_t max_bytes{100u << 20};
};

// 上传后的文档句柄。id 由厂商给，后续轮次只回传它。
struct UploadedDoc {
  std::string file_id;
  std::string filename;
  std::size_t bytes{0};
  Mode mode{Mode::kText};
};

class IDocumentParser {
 public:
  virtual ~IDocumentParser() = default;

  // 上传 + 解析，返回厂商侧的 file_id。
  // filename 用于让上游按扩展名选解析器，必须准。
  virtual fish::core::Result<UploadedDoc> upload(const std::string& path,
                                                 const std::string& filename,
                                                 Mode mode) = 0;

  // 直接取纯文本（适用于 txt/md/csv，或不需要跨轮保留的小文档）。
  virtual fish::core::Result<std::string> extract_text(const std::string& path,
                                                       const std::string& filename) = 0;

  [[nodiscard]] virtual bool available() const noexcept = 0;
};

// 按配置造一个解析器。provider=none 时返回 nullptr（调用方据此降级）。
std::unique_ptr<IDocumentParser> make_parser(const Config& cfg);

Config load_config(const fish::config::Config& c);

// 该文件是否走「解析」，还是当纯文本直读。
// 表格和电子书上游会转 Markdown，也算解析。
bool needs_parsing(const std::string& filename);

}  // namespace fish::doc

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::doc {

using fish::core::ErrorCode;
using fish::core::Result;
using json = nlohmann::json;

namespace {

std::string lower_ext(const std::string& path) {
  const auto slash = path.find_last_of('/');
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  std::string ext = path.substr(dot + 1);
  for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return ext;
}

std::string mime_for(const std::string& ext) {
  if (ext == "pdf") return "application/pdf";
  if (ext == "doc") return "application/msword";
  if (ext == "docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
  if (ext == "ppt") return "application/vnd.ms-powerpoint";
  if (ext == "pptx") return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
  if (ext == "xls") return "application/vnd.ms-excel";
  if (ext == "xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "webp") return "image/webp";
  if (ext == "gif") return "image/gif";
  if (ext == "bmp") return "image/bmp";
  if (ext == "tiff" || ext == "tif") return "image/tiff";
  if (ext == "epub") return "application/epub+zip";
  return "application/octet-stream";
}

const char* purpose_of(Mode m) {
  return m == Mode::kLayout ? "rich_layout_recognition" : "text_recognition";
}

// 文件大小上限检查。max_bytes 一直存在但从来没被执行过：
// 用户发来一个 500MB 的压缩包（或误传视频）时，程序会老实把它整个读进
// 内存再上传，直接把进程撑爆。入口统一挡掉。
//
// 返回空字符串 = 通过；否则是给用户看的错误详情。
std::string check_size(const std::string& path, std::size_t max_bytes) {
  if (max_bytes == 0) return {};   // 0 = 不限制（与 guard 的口径一致）
  std::error_code ec;
  const auto sz = std::filesystem::file_size(path, ec);
  if (ec) return {};   // 量不出来（文件不存在等）交给后续 open 报错，别误报超限
  if (sz <= max_bytes) return {};
  return "doc_too_large:" + std::to_string(sz) + ">" + std::to_string(max_bytes);
}

}  // namespace

bool needs_parsing(const std::string& filename) {
  const std::string ext = lower_ext(filename);
  // 这些上游要解析（PDF/Office/图片；表格与电子书会转 Markdown）
  static const std::set<std::string> kParse{
      "pdf", "doc", "docx", "ppt", "pptx", "xls",  "xlsx", "epub", "mobi",
      "png", "jpg",  "jpeg", "webp", "gif", "bmp", "tiff", "tif"};
  return kParse.count(ext) > 0;
}

// DeepSeek Files API。
// 端点：POST {base}/files，multipart，purpose 必须在 file 之前。
// 解析在上传时完成，成功后才返回 file_id。
class DeepSeekDocs final : public IDocumentParser {
 public:
  explicit DeepSeekDocs(Config cfg) : cfg_(std::move(cfg)) {}

  Result<UploadedDoc> upload(const std::string& path, const std::string& filename,
                             Mode mode) override {
    const std::string ext = lower_ext(filename.empty() ? path : filename);
    const std::string name = filename.empty() ? ("file." + ext) : filename;

    if (auto too_big = check_size(path, cfg_.max_bytes); !too_big.empty()) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "doc", "upload_too_large")
          .field("detail", too_big)
          .field("max_bytes", static_cast<long long>(cfg_.max_bytes));
      return Result<UploadedDoc>::fail(ErrorCode::kBadRequest, too_big);
    }

    // 纯文本/代码类不需要解析，用 user_data 直接带进去，且不计费
    const bool plain = !needs_parsing(name);

    // 有序字段：purpose 必须排在 file 前面，否则上游 400
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("purpose",
                        plain ? "user_data" : purpose_of(mode));

    std::vector<fish::capability::HttpRequest::FormFile> files;
    files.push_back({"file", path, mime_for(ext), name});

    std::map<std::string, std::string> headers;
    if (!cfg_.api_key.empty()) headers["Authorization"] = "Bearer " + cfg_.api_key;

    auto resp = fish::capability::HttpClient::instance().post_form(
        base() + "/files", headers, fields, files, cfg_.timeout_ms);
    if (!resp.ok()) return Result<UploadedDoc>::fail(resp.error.code, resp.error.detail);

    try {
      const auto body = json::parse(resp.value->body);
      if (!body.contains("id")) {
        return Result<UploadedDoc>::fail(ErrorCode::kBadRequest, "files_no_id");
      }
      UploadedDoc d;
      d.file_id = body["id"].get<std::string>();
      d.filename = body.value("filename", name);
      d.bytes = body.value("bytes", static_cast<std::size_t>(0));
      d.mode = mode;
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "doc", "uploaded")
          .field("file_id", d.file_id)
          .field("bytes", static_cast<long long>(d.bytes))
          .field("purpose", plain ? std::string("user_data") : std::string(purpose_of(mode)));
      return Result<UploadedDoc>::ok(std::move(d));
    } catch (const std::exception& e) {
      return Result<UploadedDoc>::fail(ErrorCode::kBadRequest,
                                       std::string("files_parse:") + e.what());
    }
  }

  // 纯文本直读。上游对 txt/md/csv 不做解析，我们本地读更快也更可控。
  Result<std::string> extract_text(const std::string& path,
                                   const std::string& filename) override {
    (void)filename;
    if (auto too_big = check_size(path, cfg_.max_bytes); !too_big.empty()) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "doc", "extract_too_large")
          .field("detail", too_big)
          .field("max_bytes", static_cast<long long>(cfg_.max_bytes));
      return Result<std::string>::fail(ErrorCode::kBadRequest, too_big);
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return Result<std::string>::fail(ErrorCode::kBadRequest, "doc_unreadable");
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    if (text.empty()) {
      return Result<std::string>::fail(ErrorCode::kBadRequest, "doc_empty");
    }
    return Result<std::string>::ok(std::move(text));
  }

  bool available() const noexcept override { return !cfg_.api_key.empty(); }

 private:
  std::string base() const {
    std::string b = cfg_.base_url.empty() ? "https://api.deepseek.com" : cfg_.base_url;
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
  }

  Config cfg_;
};

std::unique_ptr<IDocumentParser> make_parser(const Config& cfg) {
  if (cfg.provider == "none" || cfg.api_key.empty()) return nullptr;
  if (cfg.provider == "deepseek") {
    return std::make_unique<DeepSeekDocs>(cfg);
  }
  ::fish::log::log_line(::fish::log::LogLevel::kWarn, "doc", "unknown_provider")
      .field("provider", cfg.provider);
  return nullptr;
}

Config load_config(const fish::config::Config& c) {
  Config cfg;
  cfg.provider = c.get_or("capability.document.provider", "none");
  cfg.base_url = c.get_or("capability.document.base_url", "https://api.deepseek.com");
  // 走共享密钥解析，别直读 capability.document.api_key。
  //
  // 以前直读 → 「只在对话里填了 DeepSeek key」时文档解析静默不启用
  // （make_parser 返回 nullptr，而 main 连日志都不打），用户只知道
  // 「发 PDF 没反应」。现在复用同提供方在 chat/search 等能力里填过的 key。
  cfg.api_key =
      fish::capability::resolve_provider_key_for(c, "capability.document", cfg.provider);
  cfg.timeout_ms =
      static_cast<int>(c.get_int_or("capability.document.timeout_ms", 180000));
  cfg.max_bytes = static_cast<std::size_t>(
      c.get_int_or("capability.document.max_bytes", 100 * 1024 * 1024));
  return cfg;
}

}  // namespace fish::doc
