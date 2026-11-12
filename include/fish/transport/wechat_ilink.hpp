// include/fish/transport/wechat_ilink.hpp
// 微信 iLink 通道（腾讯官方 openclaw-weixin 接口）。
// 仅支持单聊文本，24h 需重扫码。
#pragma once

#include <deque>
#include <mutex>
#include <string>

#include "fish/core/interfaces.hpp"

namespace fish::transport {

struct WeChatConfig {
  std::string base_url{"https://ilinkai.weixin.qq.com"};
  std::string bot_token;
  std::string bot_base_url;
  std::string channel_version{"1.0.2"};
};

class WeChatIlinkChannel final : public core::IChannel {
 public:
  explicit WeChatIlinkChannel(WeChatConfig cfg);
  ~WeChatIlinkChannel() override = default;

  [[nodiscard]] core::Channel id() const noexcept override { return core::Channel::kWeChat; }
  [[nodiscard]] core::ChannelCapability capability() const noexcept override;

  core::Error connect();
  core::Result<core::InboundMessage> poll() override;
  core::Error send(const core::OutboundMessage& msg, int attempt_no) override;

  [[nodiscard]] bool connected() const noexcept { return connected_; }

 private:
  WeChatConfig cfg_;
  bool connected_{false};
  std::string updates_buf_;
  mutable std::mutex mu_;
};

}  // namespace fish::transport
