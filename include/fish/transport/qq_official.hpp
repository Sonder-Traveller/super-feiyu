// include/fish/transport/qq_official.hpp
// QQ 官方机器人通道：WebSocket 网关直连，无需 NapCat。
#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "fish/core/interfaces.hpp"

namespace fish::transport {

struct QQOfficialConfig {
  std::string app_id;
  std::string app_secret;
  std::string api_base{"https://api.sgroup.qq.com"};
  std::string token_url{"https://bots.qq.com/app/getAppAccessToken"};
  int intents{0};  // 0 表示用默认（群@ + 单聊）
  int heartbeat_ms{41250};
};

class QQOfficialChannel final : public core::IChannel {
 public:
  explicit QQOfficialChannel(QQOfficialConfig cfg);
  ~QQOfficialChannel() override;

  [[nodiscard]] core::Channel id() const noexcept override { return core::Channel::kQQ; }
  [[nodiscard]] core::ChannelCapability capability() const noexcept override;

  // 建立 WS 连接并鉴权；失败返回错误，调用方可退避重试
  core::Error connect();

  core::Result<core::InboundMessage> poll() override;
  core::Error send(const core::OutboundMessage& msg, int attempt_no) override;

  [[nodiscard]] bool connected() const noexcept { return connected_; }

 private:
  // 刷新 access_token（约 7200s 有效）
  core::Error refresh_token();
  [[nodiscard]] std::string authorized_headers() const;

  QQOfficialConfig cfg_;
  std::string access_token_;
  core::TimePoint token_expiry_{};
  std::atomic<bool> connected_{false};

  mutable std::mutex inbox_mu_;
  std::deque<core::InboundMessage> inbox_;
  std::mutex token_mu_;
};

}  // namespace fish::transport
