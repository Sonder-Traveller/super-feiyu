// include/fish/core/types.hpp
// 核心领域类型：不依赖任何具体传输协议/模型厂商。
// 设计原则：这一层不知道 QQ 存在，也不知道 DeepSeek 存在。
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fish::core {

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// 消息来源通道。新增微信/Telegram 只加枚举值，不动上层逻辑。
enum class Channel : std::uint8_t { kQQ, kWeChat };

// 会话类型。被动模式的核心判据：群聊只有被 @ 才响应。
enum class ChatKind : std::uint8_t { kPrivate, kGroup };

// 消息内容块。多模态统一表示，避免为图片/语音各开一套接口。
enum class BlockKind : std::uint8_t { kText, kImage, kVoice };

struct ContentBlock {
  BlockKind kind{BlockKind::kText};
  std::string text;        // kText 时的正文
  std::string media_url;   // kImage/kVoice 时的可下载地址
  std::string mime;        // media 的 mime，便于下游选路
};

// 规范化后的入站消息。各通道适配器负责把自己的协议差异在这里抹平。
struct InboundMessage {
  Channel channel{Channel::kQQ};
  ChatKind kind{ChatKind::kGroup};
  std::string conversation_id;  // 群号 / 单聊用户 id，通道内唯一
  std::string sender_id;
  std::string sender_name;
  std::string message_id;  // 通道原始消息 id，被动回复的凭证
  std::vector<ContentBlock> blocks;
  bool mentioned_me{false};  // 群聊判定是否被 @
  TimePoint received_at{};

  // 兼容旧接口：消息里已带 context_token 的通道（微信 iLink）需要回填
  std::string context_token;
};

// 出站消息。注意回复预算约束：通道侧会限制每条入站的回复条数。
struct OutboundMessage {
  std::string conversation_id;
  std::string reply_to_message_id;  // 被动回复必须携带
  std::vector<ContentBlock> blocks;
  std::string context_token;
};

// 通道能力声明。让编排层在发消息前就知道"这条路能不能发语音"，
// 而不是等通道返回错误再降级。
struct ChannelCapability {
  bool supports_group{true};
  bool supports_private{true};
  bool supports_image_out{true};
  bool supports_voice_out{true};
  bool supports_voice_in{true};
  // 单条入站消息允许的最大回复条数（QQ 官方：单聊 4 / 群聊 5）
  int max_replies_per_message{1};
};

// 统一错误。禁止抛异常穿过模块边界，全部走 Result。
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

}  // namespace fish::core
