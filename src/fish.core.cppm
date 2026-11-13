// src/fish.core.cppm
// core 模块：领域类型与接口契约。
// 本模块不知道 QQ 存在，也不知道 DeepSeek 存在。
module;

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module fish.core;

export namespace fish::core {

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// ── 领域类型 ────────────────────────────────────────────
enum class Channel : std::uint8_t { kQQ, kWeChat };
enum class ChatKind : std::uint8_t { kPrivate, kGroup };
enum class BlockKind : std::uint8_t { kText, kImage, kVoice };

struct ContentBlock {
  BlockKind kind{BlockKind::kText};
  std::string text;
  std::string media_url;
  std::string mime;
  // 说话人角色："user"（对方）| "assistant"（自己）。
  // 必须显式携带：只靠「对方: / 我:」文字前缀区分，
  // 模型会把双方的话混为一谈（身份混淆、把自己说的话当成对方的要求）。
  std::string role{"user"};
};

struct InboundMessage {
  Channel channel{Channel::kQQ};
  ChatKind kind{ChatKind::kGroup};
  std::string conversation_id;
  std::string sender_id;
  std::string sender_name;
  std::string message_id;
  std::vector<ContentBlock> blocks;
  bool mentioned_me{false};
  TimePoint received_at{};
  std::string context_token;
};

struct OutboundMessage {
  std::string conversation_id;
  std::string reply_to_message_id;
  std::vector<ContentBlock> blocks;
  std::string context_token;
};

struct ChannelCapability {
  bool supports_group{true};
  bool supports_private{true};
  bool supports_image_out{true};
  bool supports_voice_out{true};
  bool supports_voice_in{true};
  int max_replies_per_message{1};
};

// ── 错误模型：不用异常，跨模块一律走 Result ─────────────
enum class ErrorCode : std::uint8_t {
  kOk,
  kTimeout,
  kRateLimited,
  kUnauthorized,
  kBadRequest,
  kUpstreamUnavailable,
  kBudgetExhausted,
  kUnsupported,
  kInternal,
};

struct Error {
  ErrorCode code{ErrorCode::kOk};
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::kOk; }
  static Error none() { return {}; }
  static Error make(ErrorCode c, std::string d) { return {c, std::move(d)}; }
};

template <typename T>
struct Result {
  std::optional<T> value;
  Error error;

  [[nodiscard]] bool ok() const noexcept { return error.ok(); }
  static Result ok(T v) { return {std::move(v), Error::none()}; }
  static Result fail(ErrorCode c, std::string d) {
    return {std::nullopt, Error::make(c, std::move(d))};
  }
};

// ── 职责契约 ────────────────────────────────────────────
class IChannel {
 public:
  virtual ~IChannel() = default;
  [[nodiscard]] virtual Channel id() const noexcept = 0;
  [[nodiscard]] virtual ChannelCapability capability() const noexcept = 0;
  virtual Result<InboundMessage> poll() = 0;
  virtual Error send(const OutboundMessage& msg, int attempt_no) = 0;
};

class IChatModel {
 public:
  // 模型请求的一次工具调用
  struct ToolCall {
    std::string id;        // 上游给的调用 id，回传结果时必须带
    std::string name;
    std::string arguments; // JSON 字符串
  };

  struct Request {
    std::string system_prompt;
    std::vector<ContentBlock> history;
    std::vector<ContentBlock> user_blocks;
    int max_tokens{512};
    double temperature{1.0};
    // 可用工具的 OpenAI tools JSON 数组（空 = 不启用工具）
    std::string tools_json;
  };
  struct Response {
    std::string text;
    int prompt_tokens{0};
    int cached_tokens{0};
    int output_tokens{0};
    // 模型要求调用的工具（可能多个）
    std::vector<ToolCall> tool_calls;
    [[nodiscard]] bool wants_tools() const { return !tool_calls.empty(); }
  };
  virtual ~IChatModel() = default;
  virtual Result<Response> complete(const Request& req) = 0;
};

class IEmbeddingModel {
 public:
  virtual ~IEmbeddingModel() = default;
  virtual Result<std::vector<float>> embed(const std::string& text) = 0;
  [[nodiscard]] virtual int dimension() const noexcept = 0;
};

class ISpeechToText {
 public:
  virtual ~ISpeechToText() = default;
  virtual Result<std::string> transcribe(const std::string& media_url) = 0;
};

class ITextToSpeech {
 public:
  virtual ~ITextToSpeech() = default;
  virtual Result<std::string> synthesize(const std::string& text) = 0;
};

class IWebSearch {
 public:
  struct Hit {
    std::string title;
    std::string url;
    std::string snippet;
    double score{0.0};
  };
  virtual ~IWebSearch() = default;
  virtual Result<std::vector<Hit>> search(const std::string& query, int max_results) = 0;
};

// 生图。与 ITextToSpeech 同构：换厂商只换 provider，调用方不感知。
// 注意「读图」不走这里 —— 读图是 chat 模型的多模态能力（kImage 块直传）。
class IImageModel {
 public:
  struct Request {
    std::string prompt;
    // 输出尺寸，形如 "1024x1024"；空表示由 provider 默认
    std::string size;
    // 参考图（改图用）：本地路径、file:// 或 http(s) URL。
    // 为空表示文生图。
    std::vector<std::string> reference_images;
    std::string negative_prompt;
  };
  struct Response {
    // 生成结果：本地落盘路径（便于直接发给通道）或 http(s) URL。
    std::string image_url;
    // provider 实际出图尺寸，形如 "1024x1024"（可能因比例限制被调整）
    std::string actual_size;
    std::string model;
    // provider 侧的计费/用量信息，便于日志核对
    std::string usage_note;
  };
  virtual ~IImageModel() = default;
  virtual Result<Response> generate(const Request& req) = 0;
};

class IPersona {
 public:
  virtual ~IPersona() = default;
  // 稳定前缀：必须逐字节固定，否则前缀缓存失效、成本翻 50 倍
  [[nodiscard]] virtual std::string stable_prefix() const = 0;
  [[nodiscard]] virtual std::string dynamic_suffix(const InboundMessage& msg,
                                                   const std::string& recalled_memory) const = 0;
};

class IReplyBudget {
 public:
  virtual ~IReplyBudget() = default;
  [[nodiscard]] virtual int remaining(const std::string& message_id) const = 0;
  virtual bool consume(const std::string& message_id) = 0;
};

}  // namespace fish::core
