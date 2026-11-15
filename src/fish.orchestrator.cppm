// src/fish.orchestrator.cppm
// 编排层：唯一持有业务决策（要不要回、回什么、怎么回）。
module;
#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <chrono>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

export module fish.orchestrator;

import fish.core;
import fish.log;
import fish.memory;
import fish.session;
import fish.image;
import fish.media;
import fish.doc;
import fish.tool;
import fish.guard;
import fish.persona;

export namespace fish::orchestrator {

// 被动对话的准入判定
class ReplyPolicy {
 public:
  struct Decision {
    bool should_reply{false};
    std::string reason;
  };
  [[nodiscard]] Decision evaluate(const fish::core::InboundMessage& msg,
                                  const fish::core::ChannelCapability& cap) const;
};

// 去重：平台重连/轮询超时会重发事件
class DedupCache {
 public:
  explicit DedupCache(std::size_t capacity = 1024);
  bool seen(const std::string& key);
  std::size_t sweep();

 private:
  struct Entry {
    fish::core::TimePoint at{};
  };
  std::size_t capacity_;
  std::mutex mu_;
  std::unordered_map<std::string, Entry> seen_;
};

// 回复预算：QQ 官方限制单聊 4 条 / 群 5 条
class ReplyBudget final : public fish::core::IReplyBudget {
 public:
  explicit ReplyBudget(int limit_per_message,
                       std::chrono::seconds window = std::chrono::minutes(70));
  [[nodiscard]] int remaining(const std::string& message_id) const override;
  bool consume(const std::string& message_id) override;
  std::size_t sweep();

 private:
  struct Entry {
    int used{0};
    fish::core::TimePoint expires_at{};
  };
  int limit_;
  std::chrono::seconds window_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> entries_;
};

// 分层人格：稳定前缀（命中缓存）+ 动态尾部
class LayeredPersona final : public fish::core::IPersona {
 public:
  LayeredPersona(std::string persona_text, std::string constraints);
  [[nodiscard]] std::string stable_prefix() const override;
  [[nodiscard]] std::string dynamic_suffix(const fish::core::InboundMessage& msg,
                                           const std::string& recalled_memory) const override;

 private:
  std::string prefix_;
};

fish::core::Result<LayeredPersona> load_persona(const std::string& persona_path,
                                                const std::string& constraints_path);

// 主链路
class Pipeline {
 public:
  struct Deps {
    fish::core::IChatModel* chat{nullptr};
    fish::core::IEmbeddingModel* embedding{nullptr};
    fish::core::IPersona* persona{nullptr};
    // 人格存储：按会话取人格。非空时优先于 persona->stable_prefix()。
    //
    // 为什么要这个：persona 是启动时固定的单一文本，无法按会话区分。
    // 用户需要「一个统一默认人格 + 每个群/每个人可以单独设」，
    // 所以人格文本改由 PersonaStore 按 conv_id 解析。
    fish::persona::PersonaStore* personas{nullptr};
    fish::core::IReplyBudget* budget{nullptr};
    fish::core::ITextToSpeech* tts{nullptr};
    fish::core::ISpeechToText* stt{nullptr};
    // 文档解析（可为空：为空时文档消息降级成「收到一个文件」提示）
    fish::doc::IDocumentParser* docs{nullptr};
    // 文档引用模式：true = 上传拿 file_id 跨轮引用，false = 取文本拼进本轮
    bool doc_use_file_id{true};
    fish::core::IImageModel* image{nullptr};      // 生图（可为空）
    fish::image::PortraitStore* portraits{nullptr};  // 形象库（可为空）
    // 工具注册表：模型可主动调用（网络搜索 / 图片检索 / 记忆检索）
    fish::tool::Registry* tools{nullptr};
    // 单轮最多几次工具调用。0 = 不限制。
    //
    // 之前固定 3 轮，模型「先查清单再挑图」这种正常两步操作
    // 加上重试就容易被卡断。需要几轮该由模型自己判断——
    // 真死循环的话上游会先报错，不需要我们拍一个数字。
    int max_tool_rounds{0};
    // 安全防护
    fish::guard::InputGuard* input_guard{nullptr};
    fish::guard::OutputGuard* output_guard{nullptr};
    fish::guard::RateLimiter* rate_limiter{nullptr};
    // 媒体缓存：把入站/出站的图片语音落本地副本。
    // 平台给的 URL 是临时签名链接，过期就成死链，记忆里必须存本地路径。
    fish::media::MediaCache* media{nullptr};
    // 会话登记表（可为空）：人格页的 bot 卡片列表数据来源
    fish::session::ConversationBook* convos{nullptr};
    fish::memory::MemorySystem* memory{nullptr};
    fish::memory::Consolidator* consolidator{nullptr};
    int passive_window_ms{300000};
    // DeepSeek 服务端联网工具（deepseek_web_search / deepseek_read_url）
    // 是否开启。orchestrator 拿不到 ChatConfig，由 main 装配时从
    // capability.chat.deepseek_web_tools 读进来。开启时本地
    // web_search / web_fetch 不注册、不声明（main.cpp 里判断）。
    bool deepseek_web_tools{false};
  };

  explicit Pipeline(Deps deps);
  fish::core::Result<int> handle(fish::core::IChannel& channel,
                                 const fish::core::InboundMessage& msg);
  DedupCache& dedup() { return dedup_; }

  // 周期维护：清理预算/去重表。由 runtime 的定时线程调用。
  void maintenance();

 private:
  fish::core::Result<std::vector<fish::core::ContentBlock>> normalize_input(
      const fish::core::InboundMessage& msg);

  // 同一会话必须串行：否则两条消息并发处理会导致
  // 回复顺序错乱、记忆写入竞争、上下文不一致。
  // 用固定数量的条带锁（而非每会话一把），内存恒定。
  [[nodiscard]] std::mutex& lock_for(const std::string& conv_id) const;

  Deps deps_;
  ReplyPolicy policy_;
  DedupCache dedup_;
  static constexpr std::size_t kShards = 64;
  mutable std::array<std::mutex, kShards> shards_{};
};

// 解析回复里的图片标记，**支持多个**。
//   [PHOTO: 描述]      → spec 为描述（现场生成）
// 一次回复里可以写多行，按出现顺序全部收集，并把标记从正文里删掉。
//
// 早期只取第一个标记，等于「一次只能发一张」；用户明确要求去掉这个限制，
// 双向都要：出站能发多张，入站能收多张（入站本来就是 vector，不受限）。
//
// 导出是为了可测：这是纯函数，测试直接喂字符串断言结果。

}  // namespace fish::orchestrator

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::orchestrator {

using fish::core::ErrorCode;
using fish::core::Result;

// 常见纯文本扩展名（小写，不含点）。
// 为什么需要：fish.doc::needs_parsing() 只把 PDF/Office/图片算「需要解析」，
// 其他一律当文本用 extract_text() 直读。无扩展名的文件会被拼成 ".bin"，
// 于是二进制被整个读进 prompt（乱码 + 浪费 token）。这里给出一份白名单，
// 只有明确是文本的才允许直读。
bool is_plain_text_ext(const std::string& ext_lower) {
  static const std::set<std::string> kText{
      "txt", "md",  "markdown", "csv", "tsv",  "json", "jsonl", "log",
      "yaml", "yml", "xml",     "html", "htm", "ini",  "toml",  "cfg",
      "conf", "env", "py",      "js",   "ts",   "jsx",  "tsx",   "c",
      "h",    "cc",  "cpp",     "cxx",  "hpp",  "hh",   "go",    "rs",
      "java", "kt",  "swift",   "sql",  "sh",   "bash", "zsh",   "bat",
      "ps1",  "lua", "rb",      "php",  "css",  "scss", "tex",   "rst"};
  return kText.count(ext_lower) > 0;
}

// 文件名 → 小写扩展名（不含点）。无扩展名返回空串。
// 只认最后一段路径里的点，避免目录名里的点被当成扩展名。
std::string lower_ext_of(const std::string& name) {
  const auto slash = name.find_last_of("/\\");
  const auto dot = name.find_last_of('.');
  if (dot == std::string::npos) return {};
  if (slash != std::string::npos && dot < slash) return {};
  std::string ext = name.substr(dot + 1);
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

// 图片 mime → 本地缓存扩展名。
// 为什么不复用 fish.media 的 ext_from_mime：它在未导出的 detail 命名空间里。
// 只覆盖常见图片类型，认不出就退回 .jpg（旧行为，至少不比以前更差）。
std::string image_ext_from_mime(const std::string& mime) {
  std::string m;
  m.reserve(mime.size());
  for (const char c : mime) m += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // 有些渠道给 "image/jpeg; charset=..."，只取分号前的部分
  const auto semi = m.find(';');
  if (semi != std::string::npos) m = m.substr(0, semi);
  while (!m.empty() && (m.back() == ' ' || m.back() == '\t')) m.pop_back();
  if (m == "image/png") return ".png";
  if (m == "image/webp") return ".webp";
  if (m == "image/gif") return ".gif";
  if (m == "image/bmp") return ".bmp";
  if (m == "image/jpeg" || m == "image/jpg") return ".jpg";
  return ".jpg";
}

// 缓存扩展名 → 规范 mime，保证块里的 mime 和落盘文件一致
std::string mime_from_image_ext(const std::string& ext) {
  if (ext == ".png") return "image/png";
  if (ext == ".webp") return "image/webp";
  if (ext == ".gif") return "image/gif";
  if (ext == ".bmp") return "image/bmp";
  return "image/jpeg";   // .jpg / 未知都按 jpeg
}

// ── ReplyPolicy ─────────────────────────────────────────
ReplyPolicy::Decision ReplyPolicy::evaluate(const fish::core::InboundMessage& msg,
                                            const fish::core::ChannelCapability& cap) const {
  if (msg.kind == fish::core::ChatKind::kPrivate) {
    if (!cap.supports_private) return {false, "private_unsupported"};
    return {true, "private"};
  }
  // 群聊：被动红线——未 @ 一律不回
  if (!cap.supports_group) return {false, "group_unsupported"};
  if (!msg.mentioned_me) return {false, "group_not_mentioned"};
  return {true, "group_mentioned"};
}

// ── DedupCache ──────────────────────────────────────────
DedupCache::DedupCache(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

bool DedupCache::seen(const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = seen_.find(key);
  if (it != seen_.end()) {
    it->second.at = fish::core::Clock::now();
    return true;
  }
  if (seen_.size() >= capacity_) {
    auto oldest = seen_.begin();
    for (auto i = seen_.begin(); i != seen_.end(); ++i) {
      if (i->second.at < oldest->second.at) oldest = i;
    }
    seen_.erase(oldest);
  }
  seen_[key] = Entry{fish::core::Clock::now()};
  return false;
}

std::size_t DedupCache::sweep() {
  std::lock_guard<std::mutex> lock(mu_);
  const auto cutoff = fish::core::Clock::now() - std::chrono::minutes(5);
  std::size_t removed = 0;
  for (auto it = seen_.begin(); it != seen_.end();) {
    if (it->second.at < cutoff) {
      it = seen_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

// ── ReplyBudget ─────────────────────────────────────────
ReplyBudget::ReplyBudget(int limit_per_message, std::chrono::seconds window)
    : limit_(limit_per_message), window_(window) {}

int ReplyBudget::remaining(const std::string& message_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = entries_.find(message_id);
  if (it == entries_.end()) return limit_;
  if (fish::core::Clock::now() >= it->second.expires_at) return limit_;
  return limit_ - it->second.used;
}

bool ReplyBudget::consume(const std::string& message_id) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = fish::core::Clock::now();
  auto it = entries_.find(message_id);
  if (it == entries_.end() || now >= it->second.expires_at) {
    entries_[message_id] = Entry{1, now + window_};
    return true;
  }
  if (it->second.used >= limit_) return false;
  ++it->second.used;
  return true;
}

std::size_t ReplyBudget::sweep() {
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = fish::core::Clock::now();
  std::size_t removed = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (now >= it->second.expires_at) {
      it = entries_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

// ── LayeredPersona ──────────────────────────────────────
LayeredPersona::LayeredPersona(std::string persona_text, std::string constraints) {
  // 顺序固定：约束在前、人格在后。顺序一变历史缓存全失效
  if (constraints.empty()) {
    prefix_ = std::move(persona_text);
    return;
  }
  std::ostringstream oss;
  oss << constraints << "\n\n" << persona_text;
  prefix_ = oss.str();
}

std::string LayeredPersona::stable_prefix() const { return prefix_; }

std::string LayeredPersona::dynamic_suffix(const fish::core::InboundMessage& msg,
                                           const std::string& recalled_memory) const {
  std::ostringstream oss;

  const auto tp = msg.received_at.time_since_epoch().count() != 0 ? msg.received_at
                                                                  : fish::core::Clock::now();
  const auto t = fish::core::Clock::to_time_t(tp);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H时", &tm_buf);

  oss << "【本轮上下文】\n";
  oss << "现在时间：" << buf << "\n";
  oss << "场景：" << (msg.kind == fish::core::ChatKind::kGroup ? "群聊" : "私聊") << "\n";
  if (!msg.sender_name.empty()) oss << "对方：" << msg.sender_name << "\n";

  if (!recalled_memory.empty()) oss << "\n【相关记忆】\n" << recalled_memory << "\n";

  // 红线放最后：近因效应，执行力最强
  oss << "\n【本轮红线】只回这一条，不要主动开启新话题；不超过两句；不要暴露你是 AI。";

  return oss.str();
}

Result<LayeredPersona> load_persona(const std::string& persona_path,
                                    const std::string& constraints_path) {
  // 空文件是合法的：默认人格就是空的，由用户自己写。
  // 这里只把「打不开」当错误，不把「内容为空」当错误。
  auto read_file = [](const std::string& path) -> Result<std::string> {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Result<std::string>::fail(ErrorCode::kBadRequest, "cannot_open:" + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return Result<std::string>::ok(ss.str());
  };

  auto p = read_file(persona_path);
  if (!p.ok()) return Result<LayeredPersona>::fail(p.error.code, p.error.detail);

  // 约束文件是可选的：不配置就只用人格文件。
  // 早期把 rules.md 当必填，等于强制用户接受一套他没见过也没改过的规则；
  // AstrBot / LangBot 都不内置这种东西，人设完全交给用户。
  std::string constraints;
  if (!constraints_path.empty()) {
    auto c = read_file(constraints_path);
    if (c.ok()) constraints = std::move(*c.value);
    // 读不到就当没有，不阻断启动
  }

  return Result<LayeredPersona>::ok(LayeredPersona(std::move(*p.value), std::move(constraints)));
}

// ── Pipeline ────────────────────────────────────────────
Pipeline::Pipeline(Deps deps) : deps_(std::move(deps)) {}

std::mutex& Pipeline::lock_for(const std::string& conv_id) const {
  const std::size_t h = std::hash<std::string>{}(conv_id);
  return shards_[h % kShards];
}

void Pipeline::maintenance() {
  const auto dropped = dedup_.sweep();
  if (dropped > 0) {
    ::fish::log::log_line(::fish::log::LogLevel::kDebug, "pipeline", "dedup_swept")
        .field("dropped", static_cast<long long>(dropped));
  }
  // 会话登记表落盘。
  // ConversationBook::touch() 只改内存 + 置 dirty_（每条消息都全量写盘会把
  // 所有会话的并发消息串行在磁盘 IO 上），所以必须有人定期调 flush()，
  // 否则 data/conversations.json 永远停在启动那一刻，GUI 卡片列表不更新。
  // dirty_ 为假时 flush() 只花一次加锁，可以无脑调用。
  if (deps_.convos != nullptr) {
    deps_.convos->flush();
  }
}

// 把时间戳转成人类可读的相对/绝对描述，用于提示词。
// 模型不理解 unix 时间戳，但理解"昨天""3 小时前"。
// 超过 7 天用绝对日期，避免"很久以前"这种模糊表述。
std::string format_relative_time(std::int64_t at_ms) {
  if (at_ms <= 0) return {};
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const auto diff_s = (now_ms - at_ms) / 1000;
  if (diff_s < 0) return "刚刚";
  if (diff_s < 60) return "刚刚";
  if (diff_s < 3600) {
    return std::to_string(diff_s / 60) + " 分钟前";
  }
  if (diff_s < 86400) {
    return std::to_string(diff_s / 3600) + " 小时前";
  }
  if (diff_s < 86400 * 7) {
    return std::to_string(diff_s / 86400) + " 天前";
  }
  // 超过一周：给绝对日期
  const std::time_t t = static_cast<std::time_t>(at_ms / 1000);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday);
  return buf;
}

Result<std::vector<fish::core::ContentBlock>> Pipeline::normalize_input(
    const fish::core::InboundMessage& msg) {
  std::vector<fish::core::ContentBlock> out;
  out.reserve(msg.blocks.size());

  for (const auto& block : msg.blocks) {
    if (block.kind == fish::core::BlockKind::kText) {
      if (!block.text.empty()) out.push_back(block);
      continue;
    }
    if (block.kind == fish::core::BlockKind::kImage) {
      // 平台给的是临时签名 URL，先落本地副本再交给模型。
      // 模型支持直接读 data: URI 或本地文件；更重要的是记忆里能存住。
      fish::core::ContentBlock copy = block;
      if (deps_.media != nullptr) {
        // 扩展名要跟着真实 mime 走：写死 .jpg 会把 png/webp 存成 .jpg，
        // 下游按扩展名推断 mime 时就会把 png 标成 image/jpeg。
        const std::string ext = image_ext_from_mime(block.mime);
        auto cached = deps_.media->fetch(block.media_url, ext);
        if (cached.ok()) {
          copy.media_url = cached.value->path;
          // mime 要跟落盘扩展名一致：下游 local_file_to_data_uri 会用扩展名
          // 覆盖 mime，这里先对齐，避免出现「文件是 .png、块里写着 image/jpeg」。
          copy.mime = mime_from_image_ext(ext);
          ::fish::log::log_line(fish::log::LogLevel::kDebug, "pipeline", "image_cached")
              .field("path", cached.value->path)
              .field("reused", cached.value->reused);
        } else {
          // 下载失败仍把原始 URL 交给模型：本轮能看就先看
          ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "image_cache_failed")
              .field("detail", cached.error.detail);
        }
      }
      out.push_back(std::move(copy));
      continue;
    }
    if (block.kind == fish::core::BlockKind::kFile) {
      fish::core::ContentBlock note;
      note.kind = fish::core::BlockKind::kText;
      const std::string name = block.filename.empty() ? "文件" : block.filename;

      if (deps_.docs == nullptr) {
        note.text = "[收到文件：" + name + "，未启用文档解析]";
        out.push_back(std::move(note));
        continue;
      }

      // 扩展名决定「能不能当文本直读」。
      //
      // fish.doc::needs_parsing() 只认 PDF/Office/图片，其余一律走
      // extract_text() 直读——无扩展名的文件会被拼成 ".bin"，于是二进制
      // 被整个读进 prompt（乱码 + 烧 token）。doc 模块不归本层改，
      // 所以在这里用文本白名单挡一道：认不出是文本的，直接给提示不读内容。
      //
      // 文件名缺失时（name 是占位「文件」）退回用 media_url 的后缀判断，
      // 否则本地路径 .pdf 这类会被误判成「无扩展名」而失去解析能力。
      std::string ext_lower = lower_ext_of(name);
      if (ext_lower.empty()) ext_lower = lower_ext_of(block.media_url);
      if (ext_lower.empty() || ext_lower == "bin") {
        note.text = "[收到文件：" + name + "，格式不支持解析]";
        out.push_back(std::move(note));
        continue;
      }
      // 后续所有「按扩展名判断」的地方都用一个补齐了扩展名的名字：
      // doc 模块的 needs_parsing() / upload() 都只看文件名，名字里没扩展名
      // 就会把 PDF 当纯文本直读（同一个 bug 的另一半）。
      const std::string parse_name =
          lower_ext_of(name).empty() ? (name + "." + ext_lower) : name;

      if (!fish::doc::needs_parsing(parse_name) && !is_plain_text_ext(ext_lower)) {
        // 有扩展名但既不需要解析、也不在文本白名单里（如 .exe/.zip/.db）：
        // 同样不能当文本直读，否则就是往上下文里灌二进制。
        note.text = "[收到文件：" + name + "，格式不支持解析]";
        out.push_back(std::move(note));
        continue;
      }

      // 先把文件落到本地。平台给的是临时签名 URL，过期即死链，
      // 而解析要花几秒到几十秒，必须有自己的副本。
      std::string local_path = block.media_url;
      if (block.media_url.rfind("http://", 0) == 0 ||
          block.media_url.rfind("https://", 0) == 0) {
        if (deps_.media == nullptr) {
          note.text = "[收到文件：" + name + "，无法下载]";
          out.push_back(std::move(note));
          continue;
        }
        // ext_lower 在上面已算过（且保证非空），这里直接复用它拼扩展名，
        // 避免再用 find_last_of('.') 误把目录名里的点当扩展名。
        auto cached = deps_.media->fetch(block.media_url, "." + ext_lower);
        if (!cached.ok()) {
          note.text = "[收到文件：" + name + "，下载失败]";
          out.push_back(std::move(note));
          ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline",
                                "doc_download_failed")
              .field("detail", cached.error.detail);
          continue;
        }
        local_path = cached.value->path;
      }

      // 小文本直接读，别为几 KB 的文字走一趟上传。
      if (!fish::doc::needs_parsing(parse_name)) {
        auto txt = deps_.docs->extract_text(local_path, parse_name);
        if (txt.ok()) {
          note.text = "[文件 " + name + " 的内容]\n" + *txt.value;
        } else {
          note.text = "[收到文件：" + name + "，读取失败]";
        }
        out.push_back(std::move(note));
        continue;
      }

      // 需要解析的大文档：上传拿 file_id，正文由厂商侧保留，
      // 后续轮次只传 id，不必每轮重复塞全文。
      auto up = deps_.docs->upload(local_path, parse_name, fish::doc::Mode::kLayout);
      if (up.ok()) {
        // file_id 挂在块上，Capability 层会包成 file 内容块发给模型
        fish::core::ContentBlock file_block;
        file_block.kind = fish::core::BlockKind::kFile;
        file_block.media_url = up.value->file_id;
        file_block.filename = name;
        out.push_back(std::move(file_block));
        fish::core::ContentBlock hint;
        hint.kind = fish::core::BlockKind::kText;
        hint.text = "[对方发来了文件：" + name + "]";
        out.push_back(std::move(hint));
      } else {
        note.text = "[收到文件：" + name + "，解析失败：" +
                    up.error.detail + "]";
        out.push_back(std::move(note));
        ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "doc_parse_failed")
            .field("file", name)
            .field("detail", up.error.detail);
      }
      continue;
    }
    if (block.kind == fish::core::BlockKind::kVoice) {
      fish::core::ContentBlock note;
      note.kind = fish::core::BlockKind::kText;
      if (deps_.stt == nullptr) {
        note.text = "[语音消息，未启用转写]";
      } else {
        auto stt = deps_.stt->transcribe(block.media_url);
        if (stt.ok() && !stt.value->empty()) {
          note.text = std::move(*stt.value);
        } else if (stt.ok()) {
          // 上游返回了 200 但文本为空：多半是音频里没有可识别的人声
          //（静音、噪声、纯背景音）。空字符串直接进上下文模型会自由发挥，
          // 必须明确告知「没听清」。
          note.text = "[语音消息转写结果为空——对方可能没说话或只有背景音]";
          ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "stt_empty_result");
        } else {
          note.text = "[语音消息转写失败]";
          ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "stt_failed")
              .field("detail", stt.error.detail);
        }
        // 转写结果（成功与失败都记）：排查「模型答非所问」时，
        // 必须知道 ASR 到底输出了什么——之前成功时不打日志，
        // 模型胡说时根本分不清是 ASR 识别错还是模型发挥。
        ::fish::log::log_line(
            stt.ok() ? fish::log::LogLevel::kInfo : fish::log::LogLevel::kWarn,
            "pipeline", "stt_result")
            .field("text", note.text.substr(0, 120))
            .field("bytes", static_cast<long long>(note.text.size()));
      }
      out.push_back(std::move(note));
    }
  }

  if (out.empty()) {
    return Result<std::vector<fish::core::ContentBlock>>::fail(ErrorCode::kBadRequest,
                                                               "no_usable_block");
  }
  return Result<std::vector<fish::core::ContentBlock>>::ok(std::move(out));
}

Result<int> Pipeline::handle(fish::core::IChannel& channel,
                             const fish::core::InboundMessage& msg) {
  const auto cap = channel.capability();

  // 会话键：全链路（记忆 / 人格 / 登记表）统一用它，不要用裸 conversation_id。
  // 渠道层只放原始 openid，群聊前缀 "g:" 由 session_key() 唯一加一次；
  // 这里再各自拼一遍就会和 SessionRegistry / ConversationBook 错位。
  const std::string conv_key = fish::session::session_key(msg);

  // 0. 会话登记：GUI 人格页要展示「已有的 bot/群」卡片（头像 + 名称 +
  //    唯一标识），所以每个见过的会话都记下元信息。放这里而不是
  //    放回复路径里，是因为被静默的会话也要出现在列表里。
  //
  //    handled=false：这里还没决定要不要处理这条消息（去重 / 频率限制
  //    / 超时都还没过）。这些被丢弃的消息不该算「处理过」。
  //    真正处理完的地方（记忆落盘后）再 bump_handled。
  if (deps_.convos != nullptr) {
    deps_.convos->touch(channel.id(), msg, false);
  }

  // 1. 去重
  if (dedup_.seen(msg.message_id)) {
    ::fish::log::log_line(fish::log::LogLevel::kDebug, "pipeline", "duplicate_dropped")
        .field("msg_id", msg.message_id);
    return Result<int>::ok(0);
  }

  // 1.5 安全防护：频率限制 + 输入检查
  if (deps_.rate_limiter != nullptr && !deps_.rate_limiter->allow(conv_key)) {
    ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "rate_limited")
        .field("conversation", conv_key);
    return Result<int>::ok(0);   // 静默丢弃，不给刷屏者反馈
  }
  if (deps_.input_guard != nullptr) {
    const auto verdict = deps_.input_guard->check(msg);
    if (!verdict.allow) {
      ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "input_rejected")
          .field("rule", verdict.rule)
          .field("conversation", conv_key);
      // 有 note 就回一句；没有就静默
      if (!verdict.note.empty()) {
        fish::core::OutboundMessage reply;
        reply.conversation_id = msg.conversation_id;   // 出站用原始 id
        reply.reply_to_message_id = msg.message_id;
        reply.context_token = msg.context_token;
        // 群/私聊随消息传下去（通道层不再靠集合反推）
        reply.kind = msg.kind;
        fish::core::ContentBlock b;
        b.kind = fish::core::BlockKind::kText;
        b.text = verdict.note;
        reply.blocks.push_back(std::move(b));
        channel.send(reply, 1);
      }
      return Result<int>::ok(0);
    }
  }

  // 2. 时效检查：超过被动窗口 80% 放弃
  if (deps_.passive_window_ms > 0 && msg.received_at.time_since_epoch().count() != 0) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             fish::core::Clock::now() - msg.received_at)
                             .count();
    if (elapsed > static_cast<long long>(deps_.passive_window_ms) * 8 / 10) {
      ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline", "expired_dropped")
          .field("msg_id", msg.message_id)
          .field("elapsed_ms", static_cast<long long>(elapsed));
      return Result<int>::ok(0);
    }
  }

  // 3. 同会话串行化：拿条带锁，避免并发处理同会话消息
  std::lock_guard<std::mutex> conv_lock(lock_for(conv_key));

  // 4. 准入判定
  const auto decision = policy_.evaluate(msg, cap);
  if (!decision.should_reply) {
    ::fish::log::log_line(fish::log::LogLevel::kDebug, "pipeline", "silent")
        .field("reason", decision.reason);
    return Result<int>::ok(0);
  }

  // 5. 预算
  if (deps_.budget != nullptr && deps_.budget->remaining(msg.message_id) <= 0) {
    ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "budget_exhausted")
        .field("msg_id", msg.message_id);
    return Result<int>::fail(ErrorCode::kBudgetExhausted, "reply_budget_exhausted");
  }

  // 6. 多模态归一化
  auto input = normalize_input(msg);
  if (!input.ok()) return Result<int>::fail(input.error.code, input.error.detail);

  std::string query;
  int image_count = 0;
  for (const auto& b : *input.value) {
    if (b.kind == fish::core::BlockKind::kText) {
      if (!query.empty()) query += " ";
      query += b.text;
    } else if (b.kind == fish::core::BlockKind::kImage) {
      ++image_count;
    }
  }
  // 纯图片消息：query 为空会让向量召回/落盘拿到空串，补一个可检索的占位
  if (query.empty() && image_count > 0) {
    query = "[图片]";
  }
  // 落盘用的文本要标出「对方发了图」。
  // 图片已由媒体缓存落成本地副本，路径存进 Message::image_paths，
  // 下一轮组装历史时会重新作为多模态内容带进去，模型能一直看到。
  std::string remembered_query = query;
  std::vector<std::string> inbound_images;
  for (const auto& b : *input.value) {
    if (b.kind == fish::core::BlockKind::kImage && !b.media_url.empty()) {
      inbound_images.push_back(b.media_url);
    }
  }
  if (!inbound_images.empty()) {
    if (remembered_query == "[图片]") {
      remembered_query = "[对方发来一张图片]";
    } else {
      remembered_query += " [对方发来一张图片]";
    }
  }

  // 7. 记忆召回
  // 7. 若原始对话超出 token 预算 → 把最早的压缩成摘要 + 落盘
  //    触发条件是「超预算」，不是轮数。
  //
  // 事实沉淀（facts/traits）由 compact() 内部对被压缩的那批 transcript
  // 追加调用一次 consolidate()——只有那里还拿得到那批原文。
  // 放在 compact 内部而不是这里的另一个理由：consolidate 要一次 LLM 调用，
  // 塞进编排层会多一层失败分支；compact 内部失败也只打 warn，不影响压缩结果。
  if (deps_.memory != nullptr && deps_.consolidator != nullptr) {
    if (deps_.memory->needs_compaction(conv_key)) {
      auto c = deps_.memory->compact(conv_key, *deps_.consolidator);
      if (!c.ok()) {
        ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "compact_failed")
            .field("detail", c.error.detail);
      }
    }
  }

  fish::memory::MemoryContext mem_ctx;
  if (deps_.memory != nullptr) {
    // gather 只按会话取「预算内的最近原文 + 滚动摘要」，不做相关性检索，
    // 所以不需要 query。相关性召回是 search_memory 工具的职责
    // （模型自己决定何时调），不是每轮自动发生的。
    auto g = deps_.memory->gather(conv_key);
    if (g.ok()) mem_ctx = std::move(*g.value);
  }

  // 8. 提示词分层：摘要 + 预算内全量原文
  std::vector<fish::core::ContentBlock> history;
  // 历史里每条 assistant 消息对应的 server_tool_calls 原始 JSON，
  // 按 assistant 消息出现顺序一一对应（空串 = 那条消息没有服务端工具调用）。
  //
  // DeepSeek 的服务端联网工具要求把上一轮的 server_tool_calls **原样**
  // 发回去，否则之前 read_link 读过的网页正文（signature）不在上下文里。
  std::vector<std::string> assistant_server_tools;
  if (!mem_ctx.summary.empty()) {
    fish::core::ContentBlock b;
    b.kind = fish::core::BlockKind::kText;
    b.text = "【更早的对话摘要】\n" + mem_ctx.summary;
    b.role = "user";
    history.push_back(std::move(b));
  }
  for (const auto& m : mem_ctx.working) {
    fish::core::ContentBlock b;
    b.kind = fish::core::BlockKind::kText;
    b.text = m.text;
    // 角色必须显式传递：模型据此区分谁说的
    b.role = (m.role == "user") ? "user" : "assistant";

    // 服务端联网工具的留痕在独立字段里（Message::server_tool_calls_json），
    // 正文永远是干净的：signature 是加密串，塞进正文只会污染上下文、
    // 白烧 token。
    if (b.role == "assistant") {
      assistant_server_tools.push_back(m.server_tool_calls_json);
    }

    // 发言人 + 时间：群聊里必须让模型知道「是谁、什么时候说的」，
    // 否则多人对话会糊成一片，也理解不了"昨天""刚才"这类指代。
    if (b.role == "user") {
      b.speaker = m.speaker;
      b.at_ms = m.at_ms;
      const std::string who = m.speaker.empty() ? std::string{} : m.speaker.display_name();
      const std::string when = format_relative_time(m.at_ms);
      if (!who.empty() || !when.empty()) {
        std::string tag;
        if (!who.empty()) tag += who;
        if (!when.empty()) {
          if (!tag.empty()) tag += "，";
          tag += when;
        }
        b.text = "[" + tag + "] " + b.text;
      }
    }
    history.push_back(std::move(b));

    // 图片按发送者归属回传，role 标明是谁发的。
    // 具体怎么编码成请求（assistant 图是否降级成单独的 user 消息 + 归属标签）
    // 由 fish.capability 根据上游能力决定，这里只如实传递归属。
    const std::string img_role = (m.role == "user") ? "user" : "assistant";
    for (const auto& path : m.image_paths) {
      if (path.empty()) continue;
      fish::core::ContentBlock img;
      img.kind = fish::core::BlockKind::kImage;
      img.media_url = path;
      img.mime = "image/png";
      img.role = img_role;
      img.speaker = m.speaker;
      history.push_back(std::move(img));
    }
  }

  // 9. 模型调用（带降级 + 工具循环）
  fish::core::IChatModel::Request req;
  // system 只放稳定前缀：它必须逐字节不变，否则前缀缓存全失效（成本差 50 倍）。
  //
  // 人格按会话解析：会话覆盖 → bot 覆盖（渠道级）→ 全局默认 → 空。
  // bot 覆盖的 key 是登记表里 bot 卡片的 key（"qq:<appid>" / "wx:<token>"），
  // GUI 人格页点 bot 卡片写的就是这份；它对该渠道的所有会话生效。
  // 空人格就不发 system 消息，模型以裸状态说话，由用户自己填。
  //
  // 这里必须用 has_override() 判定，不能拿 persona_for() 的返回值判空：
  // persona_for() 在「无会话覆盖」时会回退到全局默认（通常非空），
  // 于是「非空即用会话人格」的分支恒命中，bot 级覆盖永远走不到。
  if (deps_.personas != nullptr) {
    if (deps_.personas->has_override(conv_key)) {
      auto p = deps_.personas->persona_for(conv_key);
      if (p.ok()) req.system_prompt = std::move(*p.value);
    } else if (deps_.convos != nullptr) {
      const std::string bk = deps_.convos->bot_key_for(channel.id());
      if (!bk.empty()) {
        // persona_for() 内部会回退到全局默认，所以这里不用再单独取默认
        auto bot = deps_.personas->persona_for(bk);
        if (bot.ok()) req.system_prompt = std::move(*bot.value);
      } else {
        auto d = deps_.personas->default_persona();
        if (d.ok()) req.system_prompt = std::move(*d.value);
      }
    } else {
      auto d = deps_.personas->default_persona();
      if (d.ok()) req.system_prompt = std::move(*d.value);
    }
  } else if (deps_.persona != nullptr) {
    req.system_prompt = deps_.persona->stable_prefix();
  }
  req.user_blocks = std::move(*input.value);
  req.history = std::move(history);
  // 历史里的服务端联网工具调用记录，按 assistant 消息顺序一一对应。
  req.assistant_server_tools_json = std::move(assistant_server_tools);

  // 动态后缀：本轮上下文（时间/场景/对方）+ 本轮红线。
  //
  // 这段原来只定义了、从未调用，等于模型不知道现在几点、是群聊还是私聊，
  // 末尾那句红线也从未生效。
  //
  // 放进 system 会污染前缀缓存，所以作为 history 的**最后一条 user 消息**注入：
  // 它跟着本轮输入走，不改变前面的稳定前缀。
  //
  // 注意：dynamic_suffix 还预留了【相关记忆】参数，但 MemoryContext 目前
  // 没有召回条目字段，传空串——不编造不存在的数据。
  if (deps_.persona != nullptr) {
    const std::string suffix = deps_.persona->dynamic_suffix(msg, "");
    if (!suffix.empty()) {
      fish::core::ContentBlock ctx_block;
      ctx_block.kind = fish::core::BlockKind::kText;
      ctx_block.text = suffix;
      ctx_block.role = "user";
      req.user_blocks.insert(req.user_blocks.begin(), std::move(ctx_block));
    }
  }

  // 工具声明：只在注册表非空时带上。
  // 按通道能力过滤掉该通道做不了的：微信的 bot 外发语音气泡客户端不渲染
  // （腾讯 iLink 限制），所以不把 send_voice 暴露给微信——模型看不到就不会调，
  // 省一次 TTS 合成，也不会出现「说了要发语音但用户什么也没收到」。
  //
  // DeepSeek 服务端联网工具开启时不再注册本地 web_search/web_fetch：
  // 两者不能混用（上游会拒或行为未定义），且服务端搜索不占本地配额。
  // 本地工具的注册由 main.cpp 按 deps.deepseek_web_tools 决定。
  if (deps_.tools != nullptr && !deps_.tools->empty()) {
    const bool voice_ok = cap.supports_voice_out;
    const bool web_tools_on = deps_.deepseek_web_tools;
    req.tools_json = deps_.tools->openai_tools_json(
        [voice_ok, web_tools_on](const std::string& name) {
          if (name == "send_voice" && !voice_ok) return false;
          // 服务端联网工具在用时，本地搜索/抓取不进声明
          if (web_tools_on &&
              (name == "web_search" || name == "web_fetch")) {
            return false;
          }
          return true;
        });
  }

  auto resp = deps_.chat->complete(req);

  // 服务端联网工具的原始记录与引用：最后一轮的为准（工具循环可能多次
  // complete，最后一次的响应才是最终回复），随 assistant 消息写进记忆。
  std::string server_tool_calls_json;
  std::string annotations_json;

  // 工具要求发出的图片/语音（send_image / send_voice），在工具循环里累加
  std::vector<std::string> tool_images;
  std::vector<std::string> tool_voices;

  // 工具循环：模型要求调用工具 → 执行 → 把结果作为 user 消息追加 → 再问
  // max_tool_rounds = 0 表示不限制轮数，由模型自己决定什么时候停。
  // 保留 64 轮这个硬兜底只为防止上游 bug 导致真正的死循环
  // （正常对话连 5 轮都用不到）。
  int tool_round = 0;
  constexpr int kHardSafetyRounds = 64;
  while (resp.ok() && resp.value->wants_tools() &&
         deps_.tools != nullptr &&
         (deps_.max_tool_rounds <= 0 ? tool_round < kHardSafetyRounds
                                     : tool_round < deps_.max_tool_rounds)) {
    ++tool_round;

    // 把「模型想调用什么」和「工具返回了什么」拼成下一轮的上下文。
    // 用 user 角色承载工具结果：上游对 tool 角色的支持不稳定，
    // 实测用 user + 明确标注更可靠。
    std::string tool_block;
    for (const auto& call : resp.value->tool_calls) {
      fish::tool::Call tc;
      tc.name = call.name;
      tc.arguments_json = call.arguments;
      // 带上会话键：有些工具要把结果写进**当前会话**的记忆
      // （lookup_meme 学到新梗 → 记进这个群的 traits/facts）。
      // 工具自己不知道在跟谁说话，所以由这里告知。
      tc.conversation_id = conv_key;

      auto outcome = deps_.tools->run(tc);
      ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline", "tool_ran")
          .field("tool", call.name)
          .field("ok", outcome.ok)
          .field("round", static_cast<long long>(tool_round))
          // 记下模型实际传的参数：排查「模型行为不对」时，
          // 分不清是它传错了还是我们的工具没按预期处理。
          .field("args", call.arguments.substr(0, 200))
          .field("imgs", static_cast<long long>(outcome.image_paths.size()));

      // 工具主动要求发出去的图片（如 send_image 找到的立绘）。
      // 由管线负责上传，模型只负责说话——否则它会把文件路径当正文念出来。
      for (const auto& p : outcome.image_paths) {
        if (!p.empty()) tool_images.push_back(p);
      }
      // 工具合成的语音（send_voice），同样交给管线发送。
      for (const auto& p : outcome.voice_paths) {
        if (!p.empty()) tool_voices.push_back(p);
      }

      tool_block += "【工具调用】" + call.name + "\n";
      tool_block += "【返回】" + outcome.content + "\n\n";

      // 把「模型自己调用了这个工具」写进历史（assistant 侧）。
      //
      // 为什么必须：工具结果以 user 消息回传时，下一轮请求里**没有**
      // 「模型调过工具」的任何痕迹（上一轮的 tool_calls 只存在于响应里，
      // 不会写进 history）。模型看到的是「用户突然发来一份图库清单」，
      // 完全不知道这是自己调用的返回 → 直接回复文字、不复调工具
      // → send_image 拿到清单后图永远发不出去（实测两次）。
      //
      // 文字化意图而不是回放 OpenAI 的 tool_calls 消息格式：
      // history 是 memory::Message/ContentBlock（纯文本），协议层不支持
      // tool 角色；文字形式对所有上游兼容。
      {
        fish::core::ContentBlock intent;
        intent.kind = fish::core::BlockKind::kText;
        intent.role = "assistant";
        intent.text = "（我调用了工具 " + call.name +
                      "，参数 " + call.arguments + "，正在等待结果）";
        req.history.push_back(std::move(intent));
      }
    }

    if (tool_block.empty()) break;

    // 追加到本轮输入，让模型基于工具结果作答
    fish::core::ContentBlock tb;
    tb.kind = fish::core::BlockKind::kText;
    tb.text = "[系统：以下是工具调用的真实结果，请据此回答，不要编造]\n" + tool_block;
    tb.role = "user";
    req.user_blocks.push_back(std::move(tb));

    // 清空 tools，避免模型反复要求调用同一工具
    // （第二轮通常应该直接给答案）
    resp = deps_.chat->complete(req);
  }

  std::string reply_text;
  if (resp.ok()) {
    reply_text = resp.value->text;
    // 服务端联网工具：记录最终轮的调用记录与引用（引用只进日志与记忆，
    // 不拼进正文——拼正文会污染对话体验，capability 层已有汇总日志）
    server_tool_calls_json = std::move(resp.value->server_tool_calls_json);
    annotations_json = std::move(resp.value->annotations_json);
    if (!server_tool_calls_json.empty()) {
      ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline",
                            "server_tools_recorded")
          .field("conversation", conv_key)
          .field("has_annotations", !annotations_json.empty());
    }
    // 工具调用后模型可能只返回空文本：给个兜底
    if (reply_text.empty() && tool_round > 0) {
      reply_text = "查了一下，没找到特别确定的说法。";
    }
  } else {
    reply_text = "我卡了一下…你刚才说什么？";
    ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "chat_degraded")
        .field("detail", resp.error.detail);
  }

  // 9.8 输出防护：脱敏 + 去链接 + 长度截断
  if (deps_.output_guard != nullptr) {
    reply_text = deps_.output_guard->sanitize(reply_text);
  }

  // 图片/语音全部走工具调用（send_image / send_voice），
  // 没有 [PHOTO]/[VOICE] 文本标记了——标记会被模型当台词抄出来
  // （实测气泡里出现字面 "[VOICE]"），工具调用是结构化的，没这个问题。
  // 模型在工具结果文本里已被告知「图/语音已发出」，这里只负责挂 media 块。

  // 记忆里记「实际发出去的东西」。用户指定的格式：[图片] 描述 / [语音] 内容。
  std::string remembered_reply = reply_text;
  for (const auto& path : tool_images) {
    // 图的 caption 模型已经在工具结果里看到；记忆存路径便于之后重读
    remembered_reply += " [图片] " + path;
  }
  for (std::size_t vi = 0; vi < tool_voices.size(); ++vi) {
    remembered_reply += " [语音] （发了一条语音）";
  }

  // 出站图片的本地路径，稍后一起写进记忆。
  // 必须存路径而不是只存文字：模型被问「描述一下这张图」时
  // 需要真的把图重新带进上下文，否则只能回答「我看不到」。
  std::vector<std::string> outbound_images;

  // 10. 落盘 + 巩固（推迟到发图之后，见下方：需要带上图片路径）


  // 11-12. 组装出站，按预算逐条发送
  std::vector<fish::core::ContentBlock> outgoing;

  // 语音优先：本轮有语音就不发文字——同一段话「文字一条+语音一条」
  // 是重复轰炸（用户明确不要）。语音内容已经涵盖 reply_text 的主体。
  // 文字只在没有任何语音时发出。
  if (tool_voices.empty()) {
    fish::core::ContentBlock text_block;
    text_block.kind = fish::core::BlockKind::kText;
    text_block.text = reply_text;
    // 只有图片、没有文字时，发一个极短的话兜底（空文本会被通道丢弃）
    if (text_block.text.empty() && !tool_images.empty()) {
      text_block.text = "喏";
    }
    if (!text_block.text.empty()) outgoing.push_back(std::move(text_block));
  }

  // 工具搜到的图片：直接发出去。
    for (const auto& path : tool_images) {
    fish::core::ContentBlock img;
    img.kind = fish::core::BlockKind::kImage;
    img.media_url = path;
    img.mime = "image/png";
    img.role = "assistant";
    outbound_images.push_back(path);
    outgoing.push_back(std::move(img));
    ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline", "photo_attached")
        .field("source", "tool");
  }
  // 语音：send_voice 工具合成的 mp3，直接挂 voice 块。
  // 群聊也发——用不用由模型通过工具调用决定，不再按群/私聊硬限制。
  for (const auto& path : tool_voices) {
    fish::core::ContentBlock voice;
    voice.kind = fish::core::BlockKind::kVoice;
    voice.media_url = path;
    voice.mime = "audio/mpeg";
    voice.role = "assistant";
    outgoing.push_back(std::move(voice));
    ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline", "voice_attached")
        .field("source", "tool");
  }

  // 出站条数上限。
  //
  // 注意：这是**平台被动回复配额**（QQ 官方单聊 4 / 群 5），
  // 不是我们自己设的「一次只能发一张图」。
  // 图片按张各占一条，所以多图会吃掉配额；配额耗尽时后面的发不出去，
  // 这属于平台硬约束，日志里会显式记 quota_truncated，不做静默截断。
  const int allow = cap.max_replies_per_message;
  if (allow > 0 && static_cast<int>(outgoing.size()) > allow) {
    ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "quota_truncated")
        .field("wanted", static_cast<long long>(outgoing.size()))
        .field("allow", static_cast<long long>(allow))
        .field("dropped", static_cast<long long>(outgoing.size() - allow));
    outgoing.resize(static_cast<std::size_t>(allow));
  }

  int sent = 0;
  for (int i = 0; i < static_cast<int>(outgoing.size()); ++i) {
    if (deps_.budget != nullptr && !deps_.budget->consume(msg.message_id)) break;

    fish::core::OutboundMessage one;
    // 出站用原始 conversation_id：渠道层按它拼平台 API 路径，
    // 前缀只属于会话键，不能带到协议层。
    one.conversation_id = msg.conversation_id;
    one.reply_to_message_id = msg.message_id;
    one.context_token = msg.context_token;
    // 群/私聊随消息传下去。通道层以前靠「收消息时登记的群 id 集合」
    // 反推，重启后/主动发言时会误判成私聊（消息发错地方）。
    one.kind = msg.kind;
    one.blocks.push_back(outgoing[static_cast<std::size_t>(i)]);

    auto err = channel.send(one, i + 1);
    if (!err.ok()) {
      ::fish::log::log_line(fish::log::LogLevel::kError, "pipeline", "send_failed")
          .field("conversation", conv_key)
          .field("detail", err.detail);
      return Result<int>::fail(err.code, err.detail);
    }
    ++sent;
  }

  ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline", "replied")
      .field("conversation", conv_key)
      .field("chunks", sent)
      .field("chars", static_cast<long long>(reply_text.size()));

  // 10. 落盘 + 巩固
  // 放在发送之后：这样能把「实际发出去的图片路径」一并写进记忆，
  // 下一轮模型重新拿到这些图，才能真正回答「描述一下这张图」。
  if (deps_.memory != nullptr) {
    fish::memory::Message user_msg;
    user_msg.role = "user";
    user_msg.text = remembered_query;
    user_msg.at = msg.received_at;
    user_msg.image_paths = std::move(inbound_images);
    // 发言人 + 时间：群聊里必须记，否则模型分不清是群里哪个人说的
    user_msg.speaker = msg.speaker();
    user_msg.at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         msg.received_at.time_since_epoch())
                         .count();

    fish::memory::Message bot_msg;
    bot_msg.role = "assistant";
    bot_msg.text = remembered_reply;
    bot_msg.at = fish::core::Clock::now();
    bot_msg.image_paths = std::move(outbound_images);
    // 服务端联网工具（deepseek_web_search / deepseek_read_url）的调用记录
    // 与引用：放进**独立字段**，正文保持干净。
    //
    // 原来是把 "\x01stc:<json>\x01ann:<json>\x01" 拼在正文末尾，
    // 后果是：这串控制字符会一起进 episodes.md、进 SQLite 向量索引、
    // 还会被喂给 consolidate 的 LLM（模型看到乱码，还白烧 token）；
    // 而且解码只截 "stc:" 不截 "ann:"，一条消息里有两个标记时 rfind
    // 找错位置。改成字段后正文干净、回传时从字段取。
    bot_msg.server_tool_calls_json = server_tool_calls_json;
    bot_msg.annotations_json = annotations_json;
    bot_msg.at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        bot_msg.at.time_since_epoch())
                        .count();

    deps_.memory->record(conv_key, user_msg, bot_msg);
    // 真正处理完了：现在才给这条会话的 handled 计数 +1。
    // （去重 / 频率限制 / 超时丢弃的消息在前面就 return 了，不该计数）
    if (deps_.convos != nullptr) {
      deps_.convos->bump_handled(conv_key);
    }
    // 记忆压缩已在 gather 前由 needs_compaction()/compact() 处理
  }

  return Result<int>::ok(sent);
}

}  // namespace fish::orchestrator