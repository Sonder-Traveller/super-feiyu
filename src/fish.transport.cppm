// src/fish.transport.cppm
// 通道层：QQ 官方 / QQ 小号(OneBot) / 微信 iLink / 本地回显。
// 只做协议翻译，不做业务判断。
module;
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

export module fish.transport;

import fish.core;
import fish.log;
import fish.capability;

export namespace fish::transport {

// 本地回显通道：无需凭据，用于端到端联调
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

// QQ 官方机器人
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
  fish::core::Error connect();
  fish::core::Result<fish::core::InboundMessage> poll() override;
  fish::core::Error send(const fish::core::OutboundMessage& msg, int attempt_no) override;
  [[nodiscard]] bool connected() const noexcept { return connected_; }

 private:
  fish::core::Error refresh_token();
  [[nodiscard]] std::string auth_header() const;

  QQOfficialConfig cfg_;
  std::string access_token_;
  fish::core::TimePoint token_expiry_{};
  bool connected_{false};
  mutable std::mutex mu_;
};

// QQ 小号（OneBot v11）—— 扩展：能以真实 QQ 号收发，不受官方被动配额限制
struct OneBotConfig {
  std::string ws_url{"ws://127.0.0.1:3001"};
  std::string access_token;
  std::string self_id;
  bool reverse_ws{false};
};

class QQOneBotChannel final : public fish::core::IChannel {
 public:
  explicit QQOneBotChannel(OneBotConfig cfg);
  ~QQOneBotChannel() override;
  [[nodiscard]] fish::core::Channel id() const noexcept override;
  [[nodiscard]] fish::core::ChannelCapability capability() const noexcept override;
  fish::core::Error connect();
  fish::core::Result<fish::core::InboundMessage> poll() override;
  fish::core::Error send(const fish::core::OutboundMessage& msg, int attempt_no) override;
  void push_event(const fish::core::InboundMessage& msg);
  fish::core::Result<fish::core::InboundMessage> parse_event(const std::string& raw_json) const;
  [[nodiscard]] bool connected() const noexcept { return connected_; }

 private:
  OneBotConfig cfg_;
  bool connected_{false};
  mutable std::mutex mu_;
  std::deque<fish::core::InboundMessage> inbox_;
};

// 微信 iLink
struct WeChatConfig {
  std::string base_url{"https://ilinkai.weixin.qq.com"};
  std::string bot_token;
  std::string bot_base_url;
  std::string channel_version{"1.0.2"};
};

class WeChatIlinkChannel final : public fish::core::IChannel {
 public:
  explicit WeChatIlinkChannel(WeChatConfig cfg);
  ~WeChatIlinkChannel() override;
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

}  // namespace fish::transport

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::transport {

using fish::core::Error;
using fish::core::ErrorCode;
using fish::core::Result;
using json = nlohmann::json;

namespace {

// ws://host:port → http://host:port（OneBot 的 HTTP API 根地址）
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
QQOfficialChannel::~QQOfficialChannel() = default;

fish::core::Channel QQOfficialChannel::id() const noexcept { return fish::core::Channel::kQQ; }

fish::core::ChannelCapability QQOfficialChannel::capability() const noexcept {
  fish::core::ChannelCapability cap;
  cap.supports_group = true;
  cap.supports_private = true;
  cap.supports_image_out = true;
  cap.supports_voice_out = true;
  cap.supports_voice_in = true;
  cap.max_replies_per_message = 4;  // 官方隐私：单聊 4 条
  return cap;
}

std::string QQOfficialChannel::auth_header() const {
  return "Bot " + cfg_.app_id + "." + access_token_;
}

Error QQOfficialChannel::refresh_token() {
  json payload{{"appId", cfg_.app_id}, {"clientSecret", cfg_.app_secret}};
  auto resp = fish::capability::HttpClient::instance().post_json(cfg_.token_url, {},
                                                                 payload.dump(), 15000);
  if (!resp.ok()) {
    ::fish::log::log_line(::fish::log::LogLevel::kError, "qq", "token_refresh_failed")
        .field("detail", resp.error.detail);
    return Error::make(resp.error.code, "qq_token");
  }
  try {
    const auto body = json::parse(resp.value->body);
    if (!body.contains("access_token")) {
      return Error::make(ErrorCode::kUnauthorized, "qq_token_no_field");
    }
    access_token_ = body["access_token"].get<std::string>();
    long long ttl = 7200;
    if (body.contains("expires_in")) {
      if (body["expires_in"].is_number()) {
        ttl = body["expires_in"].get<long long>();
      } else if (body["expires_in"].is_string()) {
        ttl = std::stoll(body["expires_in"].get<std::string>());
      }
    }
    token_expiry_ = fish::core::Clock::now() + std::chrono::seconds(ttl - 60);
    return Error::none();
  } catch (const std::exception& e) {
    return Error::make(ErrorCode::kBadRequest, std::string("qq_token_parse:") + e.what());
  }
}

Error QQOfficialChannel::connect() {
  std::lock_guard<std::mutex> lock(mu_);
  if (access_token_.empty() || fish::core::Clock::now() >= token_expiry_) {
    auto e = refresh_token();
    if (!e.ok()) return e;
  }
  connected_ = true;
  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq", "connected")
      .field("app_id", cfg_.app_id);
  return Error::none();
}

Result<fish::core::InboundMessage> QQOfficialChannel::poll() {
  // 官方 HTTPS 网关无事件拉取接口，事件由 WS 推送；
  // 此处返回 timeout，由外部 WS 线程 push 到队列（见 qq_ws 扩展）。
  return Result<fish::core::InboundMessage>::fail(ErrorCode::kTimeout, "no_event");
}

Error QQOfficialChannel::send(const fish::core::OutboundMessage& msg, int attempt_no) {
  if (!connected_) {
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
  const std::string conv = is_group ? msg.conversation_id.substr(2) : msg.conversation_id;
  const std::string url = is_group
                              ? (cfg_.api_base + "/v2/groups/" + conv + "/messages")
                              : (cfg_.api_base + "/v2/users/" + conv + "/messages");

  for (const auto& block : msg.blocks) {
    if (block.kind != fish::core::BlockKind::kText) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "qq", "media_needs_upload");
      continue;
    }
    json payload;
    payload["content"] = block.text;
    payload["msg_type"] = 0;
    payload["msg_id"] = msg.reply_to_message_id;  // 被动回复凭证
    payload["msg_seq"] = attempt_no;              // 同 msg_id 递增

    auto resp = fish::capability::HttpClient::instance().post_json(
        url, {{"Authorization", auth_header()}}, payload.dump(), 15000);
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
QQOneBotChannel::~QQOneBotChannel() = default;

fish::core::Channel QQOneBotChannel::id() const noexcept { return fish::core::Channel::kQQ; }

fish::core::ChannelCapability QQOneBotChannel::capability() const noexcept {
  fish::core::ChannelCapability cap;
  cap.supports_group = true;
  cap.supports_private = true;
  cap.supports_image_out = true;
  cap.supports_voice_out = true;
  cap.supports_voice_in = true;
  cap.max_replies_per_message = 8;  // 小号无官方配额限制
  return cap;
}

Error QQOneBotChannel::connect() {
  const std::string base = http_base_from_ws(cfg_.ws_url);
  fish::capability::HttpRequest req;
  req.method = "GET";
  req.url = base + "/get_login_info";
  req.timeout_ms = 8000;
  req.max_retries = 1;

  auto resp = fish::capability::HttpClient::instance().perform(req);
  if (!resp.ok()) {
    ::fish::log::log_line(fish::log::LogLevel::kWarn, "qq_onebot", "probe_failed")
        .field("base", base);
    return Error::make(resp.error.code, "onebot_unreachable");
  }
  connected_ = true;
  ::fish::log::log_line(fish::log::LogLevel::kInfo, "qq_onebot", "connected")
      .field("base", base);
  return Error::none();
}

void QQOneBotChannel::push_event(const fish::core::InboundMessage& msg) {
  std::lock_guard<std::mutex> lock(mu_);
  inbox_.push_back(msg);
}

Result<fish::core::InboundMessage> QQOneBotChannel::poll() {
  std::lock_guard<std::mutex> lock(mu_);
  if (inbox_.empty()) {
    return Result<fish::core::InboundMessage>::fail(ErrorCode::kTimeout, "no_event");
  }
  auto msg = inbox_.front();
  inbox_.pop_front();
  return Result<fish::core::InboundMessage>::ok(std::move(msg));
}

Result<fish::core::InboundMessage> QQOneBotChannel::parse_event(
    const std::string& raw_json) const {
  json ev;
  try {
    ev = json::parse(raw_json);
  } catch (const std::exception& e) {
    return Result<fish::core::InboundMessage>::fail(ErrorCode::kBadRequest,
                                                    std::string("bad_json:") + e.what());
  }

  if (ev.value("post_type", "") != "message") {
    return Result<fish::core::InboundMessage>::fail(ErrorCode::kUnsupported, "not_message");
  }

  fish::core::InboundMessage msg;
  msg.channel = fish::core::Channel::kQQ;
  msg.received_at = fish::core::Clock::now();

  const std::string mt = ev.value("message_type", "");
  msg.kind = (mt == "group") ? fish::core::ChatKind::kGroup : fish::core::ChatKind::kPrivate;
  msg.sender_id = std::to_string(ev.value("user_id", 0LL));
  msg.message_id = std::to_string(ev.value("message_id", 0LL));

  if (msg.kind == fish::core::ChatKind::kGroup) {
    msg.conversation_id = "g:" + std::to_string(ev.value("group_id", 0LL));
  } else {
    msg.conversation_id = msg.sender_id;
  }

  if (ev.contains("sender") && ev["sender"].is_object()) {
    msg.sender_name = ev["sender"].value("card", "");
    if (msg.sender_name.empty()) {
      msg.sender_name = ev["sender"].value("nickname", msg.sender_id);
    }
  }

  bool mentioned = false;
  if (ev.contains("message") && ev["message"].is_array()) {
    for (const auto& seg : ev["message"]) {
      const std::string type = seg.value("type", "");
      const auto& data = seg.contains("data") ? seg["data"] : json::object();

      if (type == "text") {
        fish::core::ContentBlock b;
        b.kind = fish::core::BlockKind::kText;
        b.text = data.value("text", "");
        if (!b.text.empty()) msg.blocks.push_back(std::move(b));
      } else if (type == "at") {
        const std::string qq = data.value("qq", "");
        if (!cfg_.self_id.empty() && qq == cfg_.self_id) mentioned = true;
      } else if (type == "image") {
        fish::core::ContentBlock b;
        b.kind = fish::core::BlockKind::kImage;
        b.media_url = data.value("url", data.value("file", ""));
        b.mime = "image/jpeg";
        if (!b.media_url.empty()) msg.blocks.push_back(std::move(b));
      } else if (type == "record") {
        fish::core::ContentBlock b;
        b.kind = fish::core::BlockKind::kVoice;
        b.media_url = data.value("url", data.value("file", ""));
        b.mime = "audio/silk";
        if (!b.media_url.empty()) msg.blocks.push_back(std::move(b));
      }
    }
  }

  msg.mentioned_me = mentioned;
  if (msg.blocks.empty()) {
    return Result<fish::core::InboundMessage>::fail(ErrorCode::kBadRequest, "empty_message");
  }
  return Result<fish::core::InboundMessage>::ok(std::move(msg));
}

Error QQOneBotChannel::send(const fish::core::OutboundMessage& msg, int) {
  const std::string base = http_base_from_ws(cfg_.ws_url);
  const bool is_group = msg.conversation_id.rfind("g:", 0) == 0;

  for (const auto& block : msg.blocks) {
    if (block.kind != fish::core::BlockKind::kText) {
      ::fish::log::log_line(fish::log::LogLevel::kWarn, "qq_onebot", "media_skipped");
      continue;
    }
    json payload;
    payload["message"] = block.text;
    payload["auto_escape"] = false;

    std::string endpoint;
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
                                                                   payload.dump(), 15000);
    if (!resp.ok()) {
      ::fish::log::log_line(fish::log::LogLevel::kError, "qq_onebot", "send_failed")
          .field("detail", resp.error.detail);
      return Error::make(resp.error.code, resp.error.detail);
    }
  }
  return Error::none();
}

// ── WeChatIlinkChannel ──────────────────────────────────
WeChatIlinkChannel::WeChatIlinkChannel(WeChatConfig cfg) : cfg_(std::move(cfg)) {}
WeChatIlinkChannel::~WeChatIlinkChannel() = default;

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
  if (!resp.ok()) return Result<fish::core::InboundMessage>::fail(resp.error.code,
                                                                  resp.error.detail);

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
  if (!connected_) {
    return Error::make(ErrorCode::kUpstreamUnavailable, "not_connected");
  }
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
    if (!resp.ok()) {
      return Error::make(resp.error.code, resp.error.detail);
    }
  }
  return Error::none();
}

}  // namespace fish::transport
