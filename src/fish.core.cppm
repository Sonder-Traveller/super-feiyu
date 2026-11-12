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
  struct Request {
    std::string system_prompt;
    std::vector<ContentBlock> history;
    std::vector<ContentBlock> user_blocks;
    int max_tokens{512};
    double temperature{1.0};
  };
  struct Response {
    std::string text;
    int prompt_tokens{0};
    int cached_tokens{0};
    int output_tokens{0};
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
