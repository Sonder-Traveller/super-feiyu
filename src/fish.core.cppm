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
enum class BlockKind : std::uint8_t { kText, kImage, kVoice, kFile };

// 发言人信息：让模型知道「谁在说」。
// 群聊里这一点尤其重要——同一个人可能换昵称、多个人的观点要区分。
struct Speaker {
  std::string id;          // 稳定标识（openid / QQ 号）
  std::string name;        // 昵称（会变，仅用于展示）
  // 群名片 / 群昵称（与全局昵称可能不同）
  std::string group_card;
  // 群内角色："owner" | "admin" | "member"（未知为空）
  std::string role;
  [[nodiscard]] bool empty() const { return id.empty() && name.empty(); }
  // 用于提示词的显示名：优先群名片，其次昵称，最后截断的 id
  [[nodiscard]] std::string display_name() const {
    if (!group_card.empty()) return group_card;
    if (!name.empty()) return name;
    if (id.size() > 8) return id.substr(0, 8) + "…";
    return id;
  }
};

struct ContentBlock {
  BlockKind kind{BlockKind::kText};
  std::string text;
  std::string media_url;
  std::string mime;
  // 文件名（kFile 用）。上游靠扩展名选解析器，必须带。
  std::string filename;
  // 说话人角色："user"（对方）| "assistant"（自己）。
  // 必须显式携带：只靠「对方: / 我:」文字前缀区分，
  // 模型会把双方的话混为一谈（身份混淆、把自己说的话当成对方的要求）。
  std::string role{"user"};
  // 发言人（仅 user 侧有意义；assistant 侧留空）
  Speaker speaker;
  // 该消息的发送时刻（unix 毫秒）。0 表示未知。
  // 用于让模型知道「这是什么时候说的」，能理解"昨天""刚才"这类指代。
  std::int64_t at_ms{0};
};

struct InboundMessage {
  Channel channel{Channel::kQQ};
  ChatKind kind{ChatKind::kGroup};
  std::string conversation_id;
  std::string sender_id;
  std::string sender_name;
  // 群名片 / 群内角色（QQ 群才有）
  std::string sender_group_card;
  std::string sender_role;
  // 群名（群聊才有，便于模型理解场景）
  std::string conversation_title;
  std::string message_id;
  std::vector<ContentBlock> blocks;
  bool mentioned_me{false};
  TimePoint received_at{};
  std::string context_token;

  // 汇总成 Speaker，供 pipeline 使用
  [[nodiscard]] Speaker speaker() const {
    Speaker s;
    s.id = sender_id;
    s.name = sender_name;
    s.group_card = sender_group_card;
    s.role = sender_role;
    return s;
  }
};

struct OutboundMessage {
  std::string conversation_id;
  std::string reply_to_message_id;
  std::vector<ContentBlock> blocks;
  std::string context_token;
  // 会话类型（群 / 私聊）。
  //
  // 为什么要显式携带：渠道层的 conversation_id 只放原始 openid /
  // group_id（"g:" 前缀只属于会话键），出站时没法从 id 本身判断出
  // 群还是私聊。以前靠「收到消息时把群 id 记进集合」反推，于是：
  //   - 服务重启后、群里还没发过消息时，机器人主动往群里发消息 →
  //     走 /v2/users/<group_id> → 消息发到了私聊；
  //   - QQ 官方与 QQ 小号各有一份集合，跨通道不共享。
  // 群/私聊是**消息的属性**，必须随消息一起传下去。
  //
  // 默认私聊：所有构造点都是「回复一条已知 incoming 消息」，
  // 编排层会显式填上 msg.kind。
  ChatKind kind{ChatKind::kPrivate};
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
    // 0 / 负数 = 不发送该字段，由 provider 配置或上游默认值决定
    int max_tokens{0};
    double temperature{-1.0};
    // 可用工具的 OpenAI tools JSON 数组（空 = 不启用工具）
    std::string tools_json;
    // 历史里每条 assistant 消息对应的 server_tool_calls 原始 JSON。
    //
    // DeepSeek 服务端联网工具（deepseek_web_search / deepseek_read_url）
    // 的调用记录必须**原样回传**，否则之前读过的网页正文（signature）
    // 不在上下文里。按顺序一一对应 history 里的 assistant 消息：
    // 第 i 项对应第 i 条 assistant 消息，空串表示那条消息没有
    // 服务端工具调用。
    std::vector<std::string> assistant_server_tools_json;
  };
  struct Response {
    std::string text;
    int prompt_tokens{0};
    int cached_tokens{0};
    int output_tokens{0};
    // 模型要求调用的工具（可能多个）
    std::vector<ToolCall> tool_calls;
    [[nodiscard]] bool wants_tools() const { return !tool_calls.empty(); }
    // DeepSeek 服务端联网工具（deepseek_web_search / deepseek_read_url）的
    // 原始调用记录与引用，多轮回传时必须原样发回，否则之前读过的网页
    // 不在上下文里。JSON 数组文本，空 = 本轮未用。
    std::string server_tool_calls_json;
    std::string annotations_json;
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

// 网页抓取：把 URL 的内容取回来。
// 与 IWebSearch 配套：search 找链接，fetch 读内容。
//
// 安全：实现必须做 SSRF 防护（拒绝内网地址、file:// 等），
// 因为 URL 来自模型输出，模型可能被诱导去读内部服务。
class IWebFetch {
 public:
  struct Page {
    std::string url;         // 最终 URL（可能因重定向变化）
    std::string title;       // HTML <title>（非 HTML 为空）
    std::string content;     // 正文（HTML 会尽量剥离标签）
    std::string content_type;
    int status{0};
    std::size_t bytes{0};    // 原始响应大小
  };
  virtual ~IWebFetch() = default;
  virtual Result<Page> fetch(const std::string& url) = 0;
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
