// include/fish/transport/local_channel.hpp
// 本地回显通道：不需要任何平台凭据，用于端到端联调与验收。
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "fish/core/interfaces.hpp"

namespace fish::transport {

class LocalChannel final : public core::IChannel {
 public:
  explicit LocalChannel(core::ChannelCapability cap = {});

  [[nodiscard]] core::Channel id() const noexcept override { return core::Channel::kQQ; }
  [[nodiscard]] core::ChannelCapability capability() const noexcept override { return cap_; }

  // 外部投喂一条消息
  void inject(const core::InboundMessage& msg);
  // 构造一条文本消息的便捷方法
  void inject_text(const std::string& conv_id, const std::string& text, bool group,
                   bool mentioned = true);

  core::Result<core::InboundMessage> poll() override;
  core::Error send(const core::OutboundMessage& msg, int attempt_no) override;

  // 测试观察
  [[nodiscard]] std::vector<std::string> sent_texts() const;
  [[nodiscard]] std::size_t sent_count() const;

 private:
  core::ChannelCapability cap_;
  mutable std::mutex mu_;
  std::deque<core::InboundMessage> inbox_;
  std::vector<std::string> sent_;
};

}  // namespace fish::transport
