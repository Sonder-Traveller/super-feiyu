// include/fish/transport/qq_onebot.hpp
// QQ 小号通道：通过 OneBot v11 协议接入（NapCat / Lagrange / LLOneBot 等实现）。
// 扩展能力：能以真实 QQ 号身份收发，不受官方被动回复配额限制。
#pragma once

#include <deque>
#include <mutex>
#include <string>

#include "fish/core/interfaces.hpp"

namespace fish::transport {

struct OneBotConfig {
  // 反向 WS 时这里是监听地址；正向 WS 时是要连接的服务端
  std::string ws_url{"ws://127.0.0.1:3001"};
  std::string access_token;      // 可选鉴权
  std::string self_id;           // 机器人 QQ 号，用于判定 @
  bool reverse_ws{false};        // true: 我们监听；false: 我们连接
  int listen_port{6199};
  int reconnect_ms{3000};
};

// OneBot v11 通道。采用 HTTP API 发送 + WS 收事件的结构：
// 发送走 /send_group_msg、/send_private_msg，事件走 WS。
class QQOneBotChannel final : public core::IChannel {
 public:
  explicit QQOneBotChannel(OneBotConfig cfg);
  ~QQOneBotChannel() override;

  [[nodiscard]] core::Channel id() const noexcept override { return core::Channel::kWeChat; }
  [[nodiscard]] core::ChannelCapability capability() const noexcept override;

  core::Error connect();
  core::Result<core::InboundMessage> poll() override;
  core::Error send(const core::OutboundMessage& msg, int attempt_no) override;

  // 供事件线程投递（WS 收到后调用）
  void push_event(const core::InboundMessage& msg);
  void set_connected(bool v);

  // 把 OneBot 原始 JSON 事件解析成统一消息；解析失败返回错误
  core::Result<core::InboundMessage> parse_event(const std::string& raw_json) const;

  [[nodiscard]] bool connected() const noexcept { return connected_; }

 private:
  OneBotConfig cfg_;
  bool connected_{false};
  mutable std::mutex mu_;
  std::deque<core::InboundMessage> inbox_;
};

}  // namespace fish::transport
