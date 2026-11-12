// src/fish.channels.cppm
// 生产级通道实现：把 WS 协议栈适配成 core::IChannel。
//
// 这一层是「协议」与「业务」的接缝：
//   WS 协议栈（fish.qq_ws / fish.onebot_ws）→ InboundMessage 队列
//   OutboundMessage → 平台 HTTP API
//
// 支持：
//   QQOfficialChannel  官方机器人（WS 收 + HTTP 发 + 富媒体上传）
//   QQOneBotChannel    QQ 小号（WS 收 + HTTP 发）
//   WeChatIlinkChannel 微信（HTTP 长轮询 + HTTP 发）
//   LocalChannel       本地回显（测试用）
module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

export module fish.channels;

import fish.core;
import fish.log;
import fish.capability;
import fish.qq_ws;
import fish.onebot_ws;

export namespace fish::channels {

// ── 本地回显 ────────────────────────────────────────────
class LocalChannel final : public fish::core::IChannel {
 public:
  explicit LocalChannel(fish::core::ChannelCapability cap = {});
  [[nodiscard]] fish::core::Channel id() const noexcept override;
  [[nodiscard]] fish::core::ChannelCapability capability() const noexcept override;
  void inject(const fish::core::InboundMessage& msg);
  void inject_text(const std::string& conv_id, const std::string& text, bool group,
                   bool mentioned = true);
  fish::core::Result<fish::core::InboundMessage> poll() override;
  fish::core::Error send(const fish::core::OutboundMessage& msg, int attempt_no) override;
  [[nodiscard]] std::vector<std::string> sent_texts() const;
  [[nodiscard]] std::size_t sent_count() const;

 private:
  fish::core::ChannelCapability cap_;
  mutable std::mutex mu_;
  std::deque<fish::core::InboundMessage> inbox_;
  std::vector<std::string> sent_;
};

// ── QQ 官方 ─────────────────────────────────────────────
struct QQOfficialConfig {
  std::string app_id;
  std::string app_secret;
  std::string api_base{"https://api.sgroup.qq.com"};
  std::string token_url{"https://bots.qq.com/app/getAppAccessToken"};
};

class QQOfficialChannel final : public fish::core::IChannel {
 public:
  explicit QQOfficialChannel(QQOfficialConfig cfg);
  ~QQOfficialChannel() override;
  [[nodiscard]] fish::core::Channel id() const noexcept override;
  [[nodiscard]] fish::core::ChannelCapability capability() const noexcept override;

  // 建立 WS 订阅（收消息）+ 刷新 token
  fish::core::Error connect();
  void disconnect();

  fish::core::Result<fish::core::InboundMessage> poll() override;
  fish::core::Error send(const fish::core::OutboundMessage& msg, int attempt_no) override;

  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] int received() const noexcept;

 private:
  fish::core::Error refresh_token();
  [[nodiscard]] std::string auth_header() const;
  // 上传富媒体，返回 file_info 字符串
  fish::core::Result<std::string> upload_media(const std::string& conv_id, bool is_group,
                                               const std::string& local_path,
                                               int file_type);

  QQOfficialConfig cfg_;
  std::unique_ptr<fish::qq_ws::Client> ws_;
  mutable std::mutex mu_;
  std::string access_token_;
  fish::core::TimePoint token_expiry_{};
  std::deque<fish::core::InboundMessage> inbox_;
};

// ── QQ 小号（OneBot v11）────────────────────────────────
struct OneBotConfig {
  std::string ws_url{"ws://127.0.0.1:3001"};
  std::string access_token;
  std::string self_id;
  // OneBot 的 HTTP API 与 WS 端口通常不同（NapCat 默认 HTTP 3000 / WS 3001）
  // 留空则从 ws_url 推导（同端口部署时可用）
  std::string http_url;
};

class QQOneBotChannel final : public fish::core::IChannel {
 public:
  explicit QQOneBotChannel(OneBotConfig cfg);
  ~QQOneBotChannel() override;
  [[nodiscard]] fish::core::Channel id() const noexcept override;
  [[nodiscard]] fish::core::ChannelCapability capability() const noexcept override;

  fish::core::Error connect();
  void disconnect();

  fish::core::Result<fish::core::InboundMessage> poll() override;
  fish::core::Error send(const fish::core::OutboundMessage& msg, int attempt_no) override;

  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] int received() const noexcept;

 private:
  OneBotConfig cfg_;
  std::unique_ptr<fish::onebot_ws::Client> ws_;
  mutable std::mutex mu_;
  std::deque<fish::core::InboundMessage> inbox_;
};

// ── 微信 iLink ──────────────────────────────────────────
struct WeChatConfig {
  std::string base_url{"https://ilinkai.weixin.qq.com"};
  std::string bot_token;
  std::string bot_base_url;
  std::string channel_version{"1.0.2"};
};

class WeChatIlinkChannel final : public fish::core::IChannel {
 public:
  explicit WeChatIlinkChannel(WeChatConfig cfg);
  ~WeChatIlinkChannel() override = default;
  [[nodiscard]] fish::core::Channel id() const noexcept override;
  [[nodiscard]] fish::core::ChannelCapability capability() const noexcept override;
  fish::core::Error connect();
  fish::core::Result<fish::core::InboundMessage> poll() override;
  fish::core::Error send(const fish::core::OutboundMessage& msg, int attempt_no) override;
  [[nodiscard]] bool connected() const noexcept { return connected_; }

 private:
  WeChatConfig cfg_;
  bool connected_{false};
  std::string updates_buf_;
};

// 按配置构造通道（main 用）
struct ChannelSet {
  std::vector<std::unique_ptr<fish::core::IChannel>> all;
};

}  // namespace fish::channels

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::channels {

using fish::core::Error;
using fish::core::ErrorCode;
using fish::core::Result;
using json = nlohmann::json;

namespace {

// ws://host:port → http://host:port
std::string http_base_from_ws(const std::string& ws) {
  std::string s = ws;
  if (s.rfind("ws://", 0) == 0) {
    s = "http://" + s.substr(5);
  } else if (s.rfind("wss://", 0) == 0) {
    s = "https://" + s.substr(6);
  }
  const auto slash = s.find('/', 8);
  if (slash != std::string::npos) s = s.substr(0, slash);
  return s;
}

}  // namespace

// ── LocalChannel ────────────────────────────────────────
LocalChannel::LocalChannel(fish::core::ChannelCapability cap) : cap_(cap) {}

fish::core::Channel LocalChannel::id() const noexcept { return fish::core::Channel::kQQ; }
fish::core::ChannelCapability LocalChannel::capability() const noexcept { return cap_; }

void LocalChannel::inject(const fish::core::InboundMessage& msg) {
  std::lock_guard<std::mutex> lock(mu_);
  inbox_.push_back(msg);
}

void LocalChannel::inject_text(const std::string& conv_id, const std::string& text, bool group,
                               bool mentioned) {
  fish::core::InboundMessage msg;
  msg.channel = fish::core::Channel::kQQ;
  msg.kind = group ? fish::core::ChatKind::kGroup : fish::core::ChatKind::kPrivate;
  msg.conversation_id = conv_id;
  msg.sender_id = "tester";
  msg.sender_name = "测试者";
  msg.message_id = "local-" + std::to_string(inbox_.size() + 1) + "-" + conv_id;
  msg.mentioned_me = mentioned;
  msg.received_at = fish::core::Clock::now();

  fish::core::ContentBlock block;
  block.kind = fish::core::BlockKind::kText;
  block.text = text;
  msg.blocks.push_back(std::move(block));
  inject(msg);
}

Result<fish::core::InboundMessage> LocalChannel::poll() {
  std::lock_guard<std::mutex> lock(mu_);
  if (inbox_.empty()) {
    return Result<fish::core::InboundMessage>::fail(ErrorCode::kTimeout, "no_message");
  }
  auto msg = inbox_.front();
  inbox_.pop_front();
  return Result<fish::core::InboundMessage>::ok(std::move(msg));
}

Error LocalChannel::send(const fish::core::OutboundMessage& msg, int) {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& b : msg.blocks) {
    if (b.kind == fish::core::BlockKind::kText) {
      sent_.push_back(b.text);
    } else if (b.kind == fish::core::BlockKind::kVoice) {
      sent_.push_back("[voice]" + b.media_url);
    } else if (b.kind == fish::core::BlockKind::kImage) {
      sent_.push_back("[image]" + b.media_url);
    }
  }
  return Error::none();
}

std::vector<std::string> LocalChannel::sent_texts() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sent_;
}

std::size_t LocalChannel::sent_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sent_.size();
}

// ── QQOfficialChannel ───────────────────────────────────
QQOfficialChannel::QQOfficialChannel(QQOfficialConfig cfg) : cfg_(std::move(cfg)) {}
QQOfficialChannel::~QQOfficialChannel() { disconnect(); }

fish::core::Channel QQOfficialChannel::id() const noexcept { return fish::core::Channel::kQQ; }

fish::core::ChannelCapability QQOfficialChannel::capability() const noexcept {
  fish::core::ChannelCapability cap;
  cap.supports_group = true;
  cap.supports_private = true;
  cap.supports_image_out = true;
  cap.supports_voice_out = true;
  cap.supports_voice_in = true;
  // 被动回复：单聊 4 条 / 群聊 5 条（取保守值）
  cap.max_replies_per_message = 4;
  return cap;
}

bool QQOfficialChannel::connected() const noexcept {
  return ws_ != nullptr && ws_->running();
}

int QQOfficialChannel::received() const noexcept { return ws_ ? ws_->event_count() : 0; }

std::string QQOfficialChannel::auth_header() const {
  std::lock_guard<std::mutex> lock(mu_);
  return "QQBot " + access_token_;
}

Error QQOfficialChannel::refresh_token() {
  json payload{{"appId", cfg_.app_id}, {"clientSecret", cfg_.app_secret}};
  auto resp = fish::capability::HttpClient::instance().post_json(cfg_.token_url, {},
                                                                 payload.dump(), 15000);
  if (!resp.ok()) {
    ::fish::log::log_line(::fish::log::LogLevel::kError, "qq", "token_failed")
        .field("detail", resp.error.detail);
    return Error::make(resp.error.code, "qq_token");
  }
  try {
    const auto j = json::parse(resp.value->body);
    if (!j.contains("access_token")) {
      return Error::make(ErrorCode::kUnauthorized, "qq_token_no_field");
    }
    long long ttl = 7200;
    if (j.contains("expires_in")) {
      if (j["expires_in"].is_number()) {
        ttl = j["expires_in"].get<long long>();
      } else if (j["expires_in"].is_string()) {
        ttl = std::stoll(j["expires_in"].get<std::string>());
      }
    }
    std::lock_guard<std::mutex> lock(mu_);
    access_token_ = j["access_token"].get<std::string>();
    token_expiry_ = fish::core::Clock::now() + std::chrono::seconds(ttl - 120);
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq", "token_ok")
        .field("ttl_s", ttl);
    return Error::none();
  } catch (const std::exception& e) {
    return Error::make(ErrorCode::kBadRequest, std::string("qq_token_parse:") + e.what());
  }
}

Error QQOfficialChannel::connect() {
  if (cfg_.app_id.empty() || cfg_.app_secret.empty()) {
    return Error::make(ErrorCode::kUnauthorized, "qq_app_credentials_missing");
  }
  auto e = refresh_token();
  if (!e.ok()) return e;

  fish::qq_ws::Config wc;
  wc.app_id = cfg_.app_id;
  wc.app_secret = cfg_.app_secret;
  wc.api_base = cfg_.api_base;
  wc.token_url = cfg_.token_url;
  wc.intents = static_cast<std::uint32_t>(fish::qq_ws::Intent::kGroupAndC2C);

  auto ws = std::make_unique<fish::qq_ws::Client>(wc);
  ws->on_event([this](const fish::qq_ws::Event& ev) {
    fish::core::InboundMessage msg;
    msg.channel = fish::core::Channel::kQQ;
    msg.kind = ev.is_group ? fish::core::ChatKind::kGroup : fish::core::ChatKind::kPrivate;
    msg.conversation_id = ev.conversation_id;
    msg.sender_id = ev.sender_id;
    msg.sender_name = ev.sender_name;
    msg.message_id = ev.message_id;
    msg.mentioned_me = ev.mentioned;
    msg.received_at = fish::core::Clock::now();

    if (!ev.content.empty()) {
      fish::core::ContentBlock b;
      b.kind = fish::core::BlockKind::kText;
      b.text = ev.content;
      msg.blocks.push_back(std::move(b));
    }
    if (msg.blocks.empty()) return;

    std::lock_guard<std::mutex> lock(mu_);
    inbox_.push_back(std::move(msg));
  });

  e = ws->start();
  if (!e.ok()) {
    ::fish::log::log_line(::fish::log::LogLevel::kError, "qq", "ws_start_failed")
        .field("detail", e.detail);
    return e;
  }
  ws_ = std::move(ws);

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq", "connected")
      .field("app_id", cfg_.app_id);
  return Error::none();
}

void QQOfficialChannel::disconnect() {
  if (ws_) {
    ws_->stop();
    ws_.reset();
  }
}

Result<fish::core::InboundMessage> QQOfficialChannel::poll() {
  std::lock_guard<std::mutex> lock(mu_);
  if (inbox_.empty()) {
    return Result<fish::core::InboundMessage>::fail(ErrorCode::kTimeout, "no_event");
  }
  auto msg = std::move(inbox_.front());
  inbox_.pop_front();
  return Result<fish::core::InboundMessage>::ok(std::move(msg));
}

Result<std::string> QQOfficialChannel::upload_media(const std::string& conv_id, bool is_group,
                                                    const std::string& local_path,
                                                    int file_type) {
  // file_type: 1=图片 2=视频 3=语音 4=文件
  std::ifstream in(local_path, std::ios::binary | std::ios::ate);
  if (!in) {
    return Result<std::string>::fail(ErrorCode::kBadRequest, "media_open_failed");
  }
  const auto size = in.tellg();
  if (size <= 0) {
    return Result<std::string>::fail(ErrorCode::kBadRequest, "media_empty");
  }
  in.seekg(0);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  in.read(bytes.data(), size);

  // 平台要求先上传拿 file_info；用 multipart 提交
  // 注意：为保持依赖精简，这里用 curl 命令行调用 multipart 上传
  const std::string tmp = "/tmp/fish_upload_" + std::to_string(std::rand());
  {
    std::ofstream f(tmp, std::ios::binary);
    if (!f) return Result<std::string>::fail(ErrorCode::kInternal, "tmp_write");
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  std::string conv = conv_id;
  if (is_group && conv.rfind("g:", 0) == 0) conv = conv.substr(2);
  const std::string url = cfg_.api_base +
                          (is_group ? "/v2/groups/" + conv + "/files"
                                    : "/v2/users/" + conv + "/files");

  const std::string cmd = "curl -s -X POST '" + url + "' "
                          "-H 'Authorization: " + auth_header() + "' " +
                          "-F 'file_type=" + std::to_string(file_type) + "' " +
                          "-F 'srv_send_msg=false' " +
                          "-F 'file=@" + tmp + "' 2>/dev/null";
  std::string out;
  if (FILE* pipe = popen(cmd.c_str(), "r")) {
    char buf[8192];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) out += buf;
    pclose(pipe);
  }
  std::remove(tmp.c_str());

  if (out.empty()) {
    return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "upload_no_response");
  }
  try {
    const auto j = json::parse(out);
    if (j.contains("file_info")) {
      return Result<std::string>::ok(j["file_info"].get<std::string>());
    }
  } catch (...) {
  }
  return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "upload_failed");
}

Error QQOfficialChannel::send(const fish::core::OutboundMessage& msg, int attempt_no) {
  if (!connected()) {
    return Error::make(ErrorCode::kUpstreamUnavailable, "qq_not_connected");
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (fish::core::Clock::now() >= token_expiry_) {
      auto e = refresh_token();
      if (!e.ok()) return e;
    }
  }

  const bool is_group = msg.conversation_id.rfind("g:", 0) == 0;
  std::string conv = is_group ? msg.conversation_id.substr(2) : msg.conversation_id;
  const std::string url = cfg_.api_base +
                          (is_group ? "/v2/groups/" + conv + "/messages"
                                    : "/v2/users/" + conv + "/messages");

  for (const auto& block : msg.blocks) {
    json payload;
    payload["msg_id"] = msg.reply_to_message_id;  // 被动回复凭证
    payload["msg_seq"] = attempt_no;              // 同 msg_id 递增

    if (block.kind == fish::core::BlockKind::kText) {
      payload["content"] = block.text;
      payload["msg_type"] = 0;
    } else if (block.kind == fish::core::BlockKind::kImage ||
               block.kind == fish::core::BlockKind::kVoice) {
      // 富媒体：先上传拿 file_info
      const int file_type = (block.kind == fish::core::BlockKind::kImage) ? 1 : 3;
      auto info = upload_media(msg.conversation_id, is_group, block.media_url, file_type);
      if (!info.ok()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "qq", "media_upload_failed")
            .field("detail", info.error.detail);
        continue;  // 跳过该段，不影响文本发送
      }
      payload["msg_type"] = 7;
      payload["media"] = {{"file_info", *info.value}};
    } else {
      continue;
    }

    auto resp = fish::capability::HttpClient::instance().post_json(
        url, {{"Authorization", auth_header()}}, payload.dump(), 20000);
    if (!resp.ok()) {
      ::fish::log::log_line(::fish::log::LogLevel::kError, "qq", "send_failed")
          .field("detail", resp.error.detail);
      return Error::make(resp.error.code, resp.error.detail);
    }
  }
  return Error::none();
}

// ── QQOneBotChannel ─────────────────────────────────────
QQOneBotChannel::QQOneBotChannel(OneBotConfig cfg) : cfg_(std::move(cfg)) {}
QQOneBotChannel::~QQOneBotChannel() { disconnect(); }

fish::core::Channel QQOneBotChannel::id() const noexcept { return fish::core::Channel::kQQ; }

fish::core::ChannelCapability QQOneBotChannel::capability() const noexcept {
  fish::core::ChannelCapability cap;
  cap.supports_group = true;
  cap.supports_private = true;
  cap.supports_image_out = true;
  cap.supports_voice_out = true;
  cap.supports_voice_in = true;
  cap.max_replies_per_message = 8;  // 小号无官方配额
  return cap;
}

bool QQOneBotChannel::connected() const noexcept {
  return ws_ != nullptr && ws_->running();
}

int QQOneBotChannel::received() const noexcept { return ws_ ? ws_->event_count() : 0; }

Error QQOneBotChannel::connect() {
  fish::onebot_ws::Config wc;
  wc.ws_url = cfg_.ws_url;
  wc.access_token = cfg_.access_token;
  wc.self_id = cfg_.self_id;

  auto ws = std::make_unique<fish::onebot_ws::Client>(wc);
  ws->on_event([this](const fish::onebot_ws::Event& ev) {
    fish::core::InboundMessage msg;
    msg.channel = fish::core::Channel::kQQ;
    msg.kind = ev.is_group ? fish::core::ChatKind::kGroup : fish::core::ChatKind::kPrivate;
    msg.conversation_id = ev.is_group ? ("g:" + ev.group_id) : ev.user_id;
    msg.sender_id = ev.user_id;
    msg.sender_name = ev.nickname;
    msg.message_id = ev.message_id;
    msg.mentioned_me = ev.mentioned;
    msg.received_at = fish::core::Clock::now();

    for (const auto& s : ev.segments) {
      if (s.type == "text") {
        fish::core::ContentBlock b;
        b.kind = fish::core::BlockKind::kText;
        b.text = s.text;
        msg.blocks.push_back(std::move(b));
      } else if (s.type == "image" && !s.url.empty()) {
        fish::core::ContentBlock b;
        b.kind = fish::core::BlockKind::kImage;
        b.media_url = s.url;
        b.mime = "image/jpeg";
        msg.blocks.push_back(std::move(b));
      } else if (s.type == "record" && !s.url.empty()) {
        fish::core::ContentBlock b;
        b.kind = fish::core::BlockKind::kVoice;
        b.media_url = s.url;
        b.mime = "audio/silk";
        msg.blocks.push_back(std::move(b));
      }
    }
    if (msg.blocks.empty()) return;

    std::lock_guard<std::mutex> lock(mu_);
    inbox_.push_back(std::move(msg));
  });

  auto e = ws->start();
  if (!e.ok()) return e;
  ws_ = std::move(ws);

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq_onebot", "connected")
      .field("url", cfg_.ws_url);
  return Error::none();
}

void QQOneBotChannel::disconnect() {
  if (ws_) {
    ws_->stop();
    ws_.reset();
  }
}

Result<fish::core::InboundMessage> QQOneBotChannel::poll() {
  std::lock_guard<std::mutex> lock(mu_);
  if (inbox_.empty()) {
    return Result<fish::core::InboundMessage>::fail(ErrorCode::kTimeout, "no_event");
  }
  auto msg = std::move(inbox_.front());
  inbox_.pop_front();
  return Result<fish::core::InboundMessage>::ok(std::move(msg));
}

Error QQOneBotChannel::send(const fish::core::OutboundMessage& msg, int) {
  const std::string base =
      cfg_.http_url.empty() ? http_base_from_ws(cfg_.ws_url) : cfg_.http_url;
  const bool is_group = msg.conversation_id.rfind("g:", 0) == 0;

  for (const auto& block : msg.blocks) {
    json payload;
    std::string endpoint;

    if (block.kind == fish::core::BlockKind::kText) {
      payload["message"] = block.text;
    } else if (block.kind == fish::core::BlockKind::kImage) {
      payload["message"] = "[CQ:image,file=" + block.media_url + "]";
    } else if (block.kind == fish::core::BlockKind::kVoice) {
      payload["message"] = "[CQ:record,file=" + block.media_url + "]";
    } else {
      continue;
    }
    payload["auto_escape"] = false;

    if (is_group) {
      payload["group_id"] = std::stoll(msg.conversation_id.substr(2));
      endpoint = "/send_group_msg";
    } else {
      try {
        payload["user_id"] = std::stoll(msg.conversation_id);
      } catch (...) {
        payload["user_id"] = msg.conversation_id;
      }
      endpoint = "/send_private_msg";
    }

    std::map<std::string, std::string> headers;
    if (!cfg_.access_token.empty()) headers["Authorization"] = "Bearer " + cfg_.access_token;

    auto resp = fish::capability::HttpClient::instance().post_json(base + endpoint, headers,
                                                                   payload.dump(), 20000);
    if (!resp.ok()) {
      ::fish::log::log_line(::fish::log::LogLevel::kError, "qq_onebot", "send_failed")
          .field("detail", resp.error.detail);
      return Error::make(resp.error.code, resp.error.detail);
    }
  }
  return Error::none();
}

// ── WeChatIlinkChannel ──────────────────────────────────
WeChatIlinkChannel::WeChatIlinkChannel(WeChatConfig cfg) : cfg_(std::move(cfg)) {}

fish::core::Channel WeChatIlinkChannel::id() const noexcept { return fish::core::Channel::kWeChat; }

fish::core::ChannelCapability WeChatIlinkChannel::capability() const noexcept {
  fish::core::ChannelCapability cap;
  cap.supports_group = false;
  cap.supports_private = true;
  cap.supports_image_out = false;
  cap.supports_voice_out = false;
  cap.supports_voice_in = false;
  cap.max_replies_per_message = 100;
  return cap;
}

Error WeChatIlinkChannel::connect() {
  if (cfg_.bot_token.empty()) {
    return Error::make(ErrorCode::kUnauthorized, "wechat_token_empty");
  }
  connected_ = true;
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "wechat", "connected");
  return Error::none();
}

Result<fish::core::InboundMessage> WeChatIlinkChannel::poll() {
  if (!connected_) {
    return Result<fish::core::InboundMessage>::fail(ErrorCode::kUpstreamUnavailable,
                                                    "not_connected");
  }
  json payload{{"get_updates_buf", updates_buf_},
               {"base_info", {{"channel_version", cfg_.channel_version}}}};
  const std::string base = cfg_.bot_base_url.empty() ? cfg_.base_url : cfg_.bot_base_url;

  auto resp = fish::capability::HttpClient::instance().post_json(
      base + "/ilink/bot/getupdates",
      {{"AuthorizationType", "ilink_bot_token"}, {"Authorization", "Bearer " + cfg_.bot_token}},
      payload.dump(), 40000);
  if (!resp.ok()) {
    return Result<fish::core::InboundMessage>::fail(resp.error.code, resp.error.detail);
  }

  try {
    const auto body = json::parse(resp.value->body);
    if (body.contains("get_updates_buf")) {
      updates_buf_ = body["get_updates_buf"].get<std::string>();
    }
    for (const auto& m : body.value("msgs", json::array())) {
      if (m.value("message_type", 0) != 1) continue;

      fish::core::InboundMessage msg;
      msg.channel = fish::core::Channel::kWeChat;
      msg.kind = fish::core::ChatKind::kPrivate;
      msg.sender_id = m.value("from_user_id", "");
      msg.conversation_id = msg.sender_id;
      msg.sender_name = msg.sender_id;
      msg.message_id = m.value("msg_id", m.value("client_id", ""));
      msg.context_token = m.value("context_token", "");
      msg.received_at = fish::core::Clock::now();

      for (const auto& it : m.value("item_list", json::array())) {
        if (it.value("type", 0) == 1 && it.contains("text_item")) {
          fish::core::ContentBlock b;
          b.kind = fish::core::BlockKind::kText;
          b.text = it["text_item"].value("text", "");
          if (!b.text.empty()) msg.blocks.push_back(std::move(b));
        }
      }
      if (msg.blocks.empty()) continue;
      return Result<fish::core::InboundMessage>::ok(std::move(msg));
    }
  } catch (const std::exception& e) {
    return Result<fish::core::InboundMessage>::fail(ErrorCode::kBadRequest,
                                                    std::string("wechat_parse:") + e.what());
  }
  return Result<fish::core::InboundMessage>::fail(ErrorCode::kTimeout, "no_message");
}

Error WeChatIlinkChannel::send(const fish::core::OutboundMessage& msg, int) {
  if (!connected_) return Error::make(ErrorCode::kUpstreamUnavailable, "not_connected");

  for (const auto& block : msg.blocks) {
    if (block.kind != fish::core::BlockKind::kText) continue;
    if (msg.context_token.empty()) {
      return Error::make(ErrorCode::kBadRequest, "wechat_missing_context_token");
    }
    json payload{
        {"msg",
         {{"from_user_id", ""},
          {"to_user_id", msg.conversation_id},
          {"client_id", "openclaw-weixin-" + std::to_string(std::rand())},
          {"message_type", 2},
          {"message_state", 2},
          {"context_token", msg.context_token},
          {"item_list", json::array({{{"type", 1}, {"text_item", {{"text", block.text}}}}})}}},
        {"base_info", {{"channel_version", cfg_.channel_version}}}};

    const std::string base = cfg_.bot_base_url.empty() ? cfg_.base_url : cfg_.bot_base_url;
    auto resp = fish::capability::HttpClient::instance().post_json(
        base + "/ilink/bot/sendmessage",
        {{"AuthorizationType", "ilink_bot_token"},
         {"Authorization", "Bearer " + cfg_.bot_token}},
        payload.dump(), 20000);
    if (!resp.ok()) return Error::make(resp.error.code, resp.error.detail);
  }
  return Error::none();
}

}  // namespace fish::channels



