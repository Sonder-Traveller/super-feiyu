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
#include <random>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

#include <openssl/evp.h>
#include <openssl/md5.h>

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

// 标准 base64 编码（RFC 4648，带 = 填充）。
// QQ 官方的二进制直传要求 file_data 是这个格式。
//
// 注意：必须先转 unsigned char 再提升到 unsigned。
// char 在多数平台是有符号的，PNG 等二进制里有大量 >127 的字节，
// 直接 static_cast<unsigned>(char) 会做符号扩展（0x80 → 0xFFFFFF80），
// 污染位运算，产出的 base64 长度正确但内容全错，
// 腾讯会报 850019「富媒体文件格式不支持」。
std::string base64_encode(const std::string& in) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);

  const auto byte_at = [&in](std::size_t idx) -> unsigned {
    return static_cast<unsigned>(static_cast<unsigned char>(in[idx]));
  };

  std::size_t i = 0;
  while (i + 2 < in.size()) {
    const unsigned n = (byte_at(i) << 16) | (byte_at(i + 1) << 8) | byte_at(i + 2);
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += kTable[(n >> 6) & 0x3F];
    out += kTable[n & 0x3F];
    i += 3;
  }
  const std::size_t rest = in.size() - i;
  if (rest == 1) {
    const unsigned n = byte_at(i) << 16;
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += "==";
  } else if (rest == 2) {
    const unsigned n = (byte_at(i) << 16) | (byte_at(i + 1) << 8);
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += kTable[(n >> 6) & 0x3F];
    out += '=';
  }
  return out;
}

// 全局单调递增的消息序号。
// QQ 的去重键是 (msg_id, msg_seq)：同一 msg_id 下 seq 重复会被拒
// （40054005 消息被去重）。进程内单调递增可保证同一次运行内永不重复。
//
// 但**重启后计数器会归零**，可能与服务端记着的旧 (msg_id, seq) 撞上
// （尤其被动回复窗口 5 分钟内重启）。所以起点取一个随机偏移，
// 让重启后的序号落在历史区间之外。
unsigned long long next_msg_seq() {
  static std::atomic<unsigned long long> counter{0};
  static const unsigned long long base = [] {
    // 1..900000 的随机起点，远大于单次运行会发出的消息数
    std::random_device rd;
    return static_cast<unsigned long long>(rd() % 900000) + 1;
  }();
  return base + (++counter);
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
  // 注意：本函数自己管理 mu_，调用方**不得**持锁调用，
  // 否则 std::mutex 不可重入会直接死锁。
  // 历史 bug：send() 持锁调用这里，导致发图时进程永久卡住。
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
    // 提前量不能超过 ttl 本身，否则 expiry 落在过去，
    // 每次 send 都会重新取 token（QQ 偶尔只给 60s 的短 token）。
    const long long lead = std::min<long long>(120, ttl / 4);
    {
      std::lock_guard<std::mutex> lock(mu_);
      access_token_ = j["access_token"].get<std::string>();
      token_expiry_ = fish::core::Clock::now() + std::chrono::seconds(ttl - lead);
    }
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
    ::fish::log::log_line(::fish::log::LogLevel::kDebug, "qq", "event_in")
        .field("type", ev.type)
        .field("content_len", static_cast<long long>(ev.content.size()))
        .field("attachments", static_cast<long long>(ev.attachments.size()));

    fish::core::InboundMessage msg;
    msg.channel = fish::core::Channel::kQQ;
    msg.kind = ev.is_group ? fish::core::ChatKind::kGroup : fish::core::ChatKind::kPrivate;
    msg.conversation_id = ev.conversation_id;
    msg.sender_id = ev.sender_id;
    msg.sender_name = ev.sender_name;
    // 发言人元信息：群名片、群内角色、群名
    msg.sender_group_card = ev.sender_group_card;
    msg.sender_role = ev.sender_role;
    msg.conversation_title = ev.conversation_title;
    msg.message_id = ev.message_id;
    msg.mentioned_me = ev.mentioned;
    // 用事件时间；缺失则退回本地接收时间
    if (ev.at_ms > 0) {
      msg.received_at = fish::core::TimePoint(std::chrono::milliseconds(ev.at_ms));
    } else {
      msg.received_at = fish::core::Clock::now();
    }

    if (!ev.content.empty()) {
      fish::core::ContentBlock b;
      b.kind = fish::core::BlockKind::kText;
      b.text = ev.content;
      msg.blocks.push_back(std::move(b));
    }
    // 图片 / 语音附件：QQ 官方放在 attachments 里，content 可能为空
    for (const auto& att : ev.attachments) {
      fish::core::ContentBlock b;
      b.media_url = att.url;
      b.mime = att.content_type;
      if (att.is_image()) {
        b.kind = fish::core::BlockKind::kImage;
      } else if (att.is_voice()) {
        b.kind = fish::core::BlockKind::kVoice;
      } else {
        continue;  // 文件等其他类型暂不处理
      }
      msg.blocks.push_back(std::move(b));
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq", "attachment")
          .field("type", att.content_type)
          .field("bytes", static_cast<long long>(att.size))
          .field("w", static_cast<long long>(att.width))
          .field("h", static_cast<long long>(att.height));
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
                                                    const std::string& media_ref,
                                                    int file_type) {
  // file_type: 1=图片 2=视频 3=语音 4=文件
  //
  // 官方支持三种上传方式（见 /v2/groups/{id}/files 文档）：
  //   1. URL 上传   —— 需要公网可访问的 URL，腾讯自己去下载
  //   2. 分片上传   —— upload_prepare + PUT 分片 + 合并，适合大文件
  //   3. 二进制直传 —— JSON 里放 file_data(base64)，上限 5MB
  //
  // 这里用**方式 3**：不需要公网端口，部署最简单。
  // 之前误用 multipart file=@ 会报 40093007「富媒体文件下载失败」，
  // 因为官方根本不接受 multipart，只认 JSON 的 file_data / url / upload_id。
  //
  // 超过 5MB 的图（基本不会）回退到 URL 方式，由调用方保证 media_ref 是 URL。
  constexpr std::size_t kDirectMax = 5u << 20;

  std::string url_field;
  std::string file_data_b64;
  std::string file_name = "image.png";

  const bool is_url = media_ref.rfind("http://", 0) == 0 ||
                      media_ref.rfind("https://", 0) == 0;
  if (is_url) {
    url_field = media_ref;
  } else {
    std::ifstream in(media_ref, std::ios::binary | std::ios::ate);
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

    // 文件名取路径最后一段（官方要求扩展名与类型一致）
    const auto slash = media_ref.find_last_of('/');
    if (slash != std::string::npos && slash + 1 < media_ref.size()) {
      file_name = media_ref.substr(slash + 1);
    }

    if (bytes.size() > kDirectMax) {
      return Result<std::string>::fail(
          ErrorCode::kBadRequest,
          "media_too_large_for_direct_upload(" + std::to_string(bytes.size()) +
              "B>5MB)，需改用分片上传");
    }
    file_data_b64 = base64_encode(bytes);
  }

  std::string conv = conv_id;
  if (is_group && conv.rfind("g:", 0) == 0) conv = conv.substr(2);
  const std::string api_url = cfg_.api_base +
                              (is_group ? "/v2/groups/" + conv + "/files"
                                        : "/v2/users/" + conv + "/files");

  json payload{{"file_type", file_type}, {"srv_send_msg", false}};
  if (!url_field.empty()) {
    payload["url"] = url_field;
  } else {
    payload["file_name"] = file_name;
    payload["file_data"] = file_data_b64;
  }

  // 上传失败时 850019「格式不支持」很难定位，把请求元信息打出来
  // （不打 base64 本身，太长）
  ::fish::log::log_line(::fish::log::LogLevel::kDebug, "qq", "upload_req")
      .field("file_type", file_type)
      .field("file_name", file_name)
      .field("b64_len", static_cast<long long>(file_data_b64.size()))
      .field("has_url", !url_field.empty());

  auto resp = fish::capability::HttpClient::instance().post_json(
      api_url, {{"Authorization", auth_header()}}, payload.dump(), 60000);
  if (!resp.ok()) {
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "qq", "upload_failed")
        .field("detail", resp.error.detail)
        .field("bytes", static_cast<long long>(file_data_b64.size()));
    return Result<std::string>::fail(resp.error.code, "upload_" + resp.error.detail);
  }

  try {
    const auto j = json::parse(resp.value->body);
    if (j.contains("file_info") && j["file_info"].is_string()) {
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "qq", "upload_ok")
          .field("mode", url_field.empty() ? "direct" : "url")
          .field("bytes", static_cast<long long>(file_data_b64.size()));
      return Result<std::string>::ok(j["file_info"].get<std::string>());
    }
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "qq", "upload_no_file_info")
        .field("body", resp.value->body.substr(0, 300));
  } catch (const std::exception& e) {
    return Result<std::string>::fail(ErrorCode::kBadRequest,
                                     std::string("upload_parse:") + e.what());
  }
  return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "upload_no_file_info");
}

Error QQOfficialChannel::send(const fish::core::OutboundMessage& msg, int attempt_no) {
  if (!connected()) {
    return Error::make(ErrorCode::kUpstreamUnavailable, "qq_not_connected");
  }
  // 检查 token 是否要刷新：先只读判断，**不要在持锁状态下调用 refresh_token()**
  // （refresh_token 自己会加 mu_，std::mutex 不可重入 → 死锁）
  bool need_refresh = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    need_refresh = (fish::core::Clock::now() >= token_expiry_);
  }
  if (need_refresh) {
    auto e = refresh_token();
    if (!e.ok()) return e;
  }

  const bool is_group = msg.conversation_id.rfind("g:", 0) == 0;
  std::string conv = is_group ? msg.conversation_id.substr(2) : msg.conversation_id;
  const std::string url = cfg_.api_base +
                          (is_group ? "/v2/groups/" + conv + "/messages"
                                    : "/v2/users/" + conv + "/messages");

  for (const auto& block : msg.blocks) {
    json payload;
    payload["msg_id"] = msg.reply_to_message_id;  // 被动回复凭证
    // msg_seq 必须在同一 msg_id 下唯一递增。
    // 之前用 attempt_no（每个出站块都从 1 开始），
    // 导致第二条消息就被判「40054005 消息被去重」。
    // 这里用进程内单调递增的计数器，保证永不重复。
    const auto seq = next_msg_seq();
    payload["msg_seq"] = static_cast<long long>(seq);
    ::fish::log::log_line(::fish::log::LogLevel::kDebug, "qq", "send_seq")
        .field("msg_id", msg.reply_to_message_id)
        .field("msg_seq", static_cast<long long>(seq))
        .field("attempt", attempt_no);

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
  // 出站图片已实现：getuploadurl → AES-128-ECB → CDN → image_item。
  cap.supports_image_out = true;
  // 出站语音：腾讯 iLink 的 bot 外发语音气泡客户端不渲染（实测 + 社区一致），
  // 且云服务器登录微信有极高冻结风险。这里声明不支持，
  // 编排层据此把 send_voice 工具从微信的可用工具列表里过滤掉。
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

// ── 微信媒体辅助（匿名命名空间）────────────────────────
namespace {

// AES-128-ECB + PKCS7 加密（微信 CDN 上传要求）。
// 返回密文；失败返回空串。
std::string aes_128_ecb_encrypt(const std::string& plain, const unsigned char key[16]) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) return {};
  std::string out(plain.size() + 16, '\0');
  int len = 0;
  int total = 0;
  bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1;
  // PKCS7 padding 是 ECB 的默认行为
  ok = ok && EVP_CIPHER_CTX_set_padding(ctx, 1) == 1;
  ok = ok && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()), &len,
                               reinterpret_cast<const unsigned char*>(plain.data()),
                               static_cast<int>(plain.size())) == 1;
  if (ok) total = len;
  ok = ok && EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + total,
                                 &len) == 1;
  if (ok) total += len;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return {};
  out.resize(static_cast<std::size_t>(total));
  return out;
}

std::string md5_hex(const std::string& data) {
  unsigned char digest[MD5_DIGEST_LENGTH];
  ::MD5(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(MD5_DIGEST_LENGTH * 2);
  for (unsigned char c : digest) {
    out += hex[c >> 4];
    out += hex[c & 0x0F];
  }
  return out;
}

// 随机 16 字节的 hex 字符串（32 字符）。filekey / aeskey 共用。
std::string random_hex32() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (int i = 0; i < 16; ++i) {
    const auto b = static_cast<unsigned char>(rng() & 0xFF);
    out += hex[b >> 4];
    out += hex[b & 0x0F];
  }
  return out;
}

// X-WECHAT-UIN：随机 uint32 十进制字符串再 base64
std::string random_wechat_uin() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  const std::uint32_t v = static_cast<std::uint32_t>(rng() & 0xFFFFFFFFULL);
  const std::string dec = std::to_string(v);
  // 小型 base64（只编码一次，无依赖）
  static const char* table =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  const std::size_t n = dec.size();
  for (std::size_t i = 0; i < n; i += 3) {
    const int b0 = static_cast<unsigned char>(dec[i]);
    const int b1 = (i + 1 < n) ? static_cast<unsigned char>(dec[i + 1]) : 0;
    const int b2 = (i + 2 < n) ? static_cast<unsigned char>(dec[i + 2]) : 0;
    const int triple = (b0 << 16) | (b1 << 8) | b2;
    out += table[(triple >> 18) & 63];
    out += table[(triple >> 12) & 63];
    out += (i + 1 < n) ? std::string(1, table[(triple >> 6) & 63]) : "=";
    out += (i + 2 < n) ? std::string(1, table[triple & 63]) : "=";
  }
  return out;
}

// 生成全局唯一的 client_id。
//
// 踩过的坑：原来用 "openclaw-weixin-" + std::rand()，但全程没调过
// srand()，rand() 每个进程都从同一条序列开始（1804289383, 846930886…）。
// 服务重启后 client_id 就复用之前已经用过的值，微信按幂等性判定为
// 重复消息：HTTP 200 + 返回 message_id，但消息被静默丢弃，客户端收不到。
// 现在用「时间戳(ms) + 随机数」，跨进程、跨重启都不会重复。
std::string unique_client_id() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  const auto salt = static_cast<unsigned long long>(rng() & 0xFFFFFFFFULL);
  return "openclaw-weixin-" + std::to_string(ms) + "-" + std::to_string(salt);
}

}  // namespace

Error WeChatIlinkChannel::send(const fish::core::OutboundMessage& msg, int) {
  if (!connected_) return Error::make(ErrorCode::kUpstreamUnavailable, "not_connected");
  if (msg.context_token.empty()) {
    return Error::make(ErrorCode::kBadRequest, "wechat_missing_context_token");
  }

  const std::string base = cfg_.bot_base_url.empty() ? cfg_.base_url : cfg_.bot_base_url;
  const std::string auth_headers_token = cfg_.bot_token;

  // 文本和图片分开发（协议建议：一条请求只发一个 item，兼容性最好）。
  for (const auto& block : msg.blocks) {
    if (block.kind == fish::core::BlockKind::kText) {
      if (block.text.empty()) continue;
      json payload{
          {"msg",
           {{"from_user_id", ""},
            {"to_user_id", msg.conversation_id},
            {"client_id", unique_client_id()},
            {"message_type", 2},
            {"message_state", 2},
            {"context_token", msg.context_token},
            {"item_list", json::array({{{"type", 1}, {"text_item", {{"text", block.text}}}}})}}},
          {"base_info", {{"channel_version", cfg_.channel_version}}}};

      auto resp = fish::capability::HttpClient::instance().post_json(
          base + "/ilink/bot/sendmessage",
          {{"AuthorizationType", "ilink_bot_token"},
           {"Authorization", "Bearer " + auth_headers_token},
           {"X-WECHAT-UIN", random_wechat_uin()}},
          payload.dump(), 20000);
      if (!resp.ok()) return Error::make(resp.error.code, resp.error.detail);
      continue;
    }

    if (block.kind == fish::core::BlockKind::kImage) {
      // 图片：getuploadurl → AES-128-ECB 加密 → CDN 上传 → image_item。
      // media_url 是本地路径（形象库 / 生图缓存）。
      std::ifstream img_file(block.media_url, std::ios::binary);
      if (!img_file) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "wechat", "image_read_failed")
            .field("path", block.media_url);
        continue;  // 单张失败不拖累整条回复
      }
      std::string plain((std::istreambuf_iterator<char>(img_file)),
                        std::istreambuf_iterator<char>());
      if (plain.empty()) continue;

      // 1) 生成 filekey / aeskey（16 字节 → 32 hex 字符）
      const std::string filekey_hex = random_hex32();
      const std::string aeskey_hex = random_hex32();
      unsigned char aes_key[16];
      for (int i = 0; i < 16; ++i) {
        const auto byte = std::stoul(aeskey_hex.substr(i * 2, 2), nullptr, 16);
        aes_key[i] = static_cast<unsigned char>(byte);
      }

      // 2) AES-128-ECB 加密
      const std::string cipher = aes_128_ecb_encrypt(plain, aes_key);
      if (cipher.empty()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "wechat",
                              "image_encrypt_failed");
        continue;
      }

      // 3) getuploadurl
      const std::string md5hex = md5_hex(plain);
      json up_req{
          {"aeskey", aeskey_hex},
          {"filekey", filekey_hex},
          {"filesize", cipher.size()},
          {"media_type", 1},
          {"no_need_thumb", true},
          {"rawfilemd5", md5hex},
          {"rawsize", plain.size()},
          {"to_user_id", msg.conversation_id},
          {"base_info", {{"channel_version", cfg_.channel_version}}}};
      auto up_url = fish::capability::HttpClient::instance().post_json(
          base + "/ilink/bot/getuploadurl",
          {{"AuthorizationType", "ilink_bot_token"},
           {"Authorization", "Bearer " + auth_headers_token},
           {"Content-Type", "application/json"},
           {"X-WECHAT-UIN", random_wechat_uin()}},
          up_req.dump(), 20000);
      if (!up_url.ok()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "wechat",
                              "getuploadurl_failed")
            .field("detail", up_url.error.detail);
        continue;
      }
      std::string upload_param;
      try {
        const auto j = json::parse(up_url.value->body);
        upload_param = j.value("upload_param", "");
      } catch (const std::exception& e) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "wechat",
                              "upload_param_parse").field("detail", e.what());
        continue;
      }
      if (upload_param.empty()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "wechat",
                              "upload_param_empty");
        continue;
      }

      // 4) CDN 上传（POST 二进制密文）。响应头 x-encrypted-param 是关键。
      std::string cdn_url =
          "https://novac2c.cdn.weixin.qq.com/c2c/upload?encrypted_query_param=" +
          fish::capability::url_encode(upload_param) + "&filekey=" + filekey_hex;
      auto cdn = fish::capability::HttpClient::instance().perform(
          {.method = "UPLOAD",
           .custom_verb = "POST",
           .url = cdn_url,
           .headers = {{"Content-Type", "application/octet-stream"}},
           .body = cipher,
           .timeout_ms = 60000,
           .max_retries = 1});
      if (!cdn.ok()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "wechat", "cdn_upload_failed")
            .field("detail", cdn.error.detail);
        continue;
      }
      std::string encrypted_param;
      {
        // header 键统一转小写了
        const auto it = cdn.value->headers.find("x-encrypted-param");
        if (it != cdn.value->headers.end()) encrypted_param = it->second;
      }
      if (encrypted_param.empty()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "wechat",
                              "cdn_missing_encrypted_param");
        continue;
      }

      // 5) sendmessage：type=2 image_item。
      //    aes_key 用 base64(hex string) 形式（官方 openclaw 的出站约定）：
      //    先把 hex 字符串当字节，再 base64 —— 32 字节明文 → 44 字符 base64。
      std::string aes_key_b64_of_hex;
      {
        static const char* table =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const std::string& s = aeskey_hex;  // 32 个 ASCII 字符
        for (std::size_t i = 0; i < s.size(); i += 3) {
          const int b0 = static_cast<unsigned char>(s[i]);
          const int b1 = (i + 1 < s.size()) ? static_cast<unsigned char>(s[i + 1]) : 0;
          const int b2 = (i + 2 < s.size()) ? static_cast<unsigned char>(s[i + 2]) : 0;
          const int triple = (b0 << 16) | (b1 << 8) | b2;
          aes_key_b64_of_hex += table[(triple >> 18) & 63];
          aes_key_b64_of_hex += table[(triple >> 12) & 63];
          aes_key_b64_of_hex += (i + 1 < s.size()) ? std::string(1, table[(triple >> 6) & 63]) : "=";
          aes_key_b64_of_hex += (i + 2 < s.size()) ? std::string(1, table[triple & 63]) : "=";
        }
      }

      json payload{
          {"msg",
           {{"from_user_id", ""},
            {"to_user_id", msg.conversation_id},
            {"client_id", unique_client_id()},
            {"message_type", 2},
            {"message_state", 2},
            {"context_token", msg.context_token},
            {"item_list",
             json::array({{{"type", 2},
                           {"image_item",
                            {{"media",
                              {{"encrypt_query_param", encrypted_param},
                               {"aes_key", aes_key_b64_of_hex},
                               {"encrypt_type", 1}}},
                             {"mid_size", cipher.size()}}}}})}}},
          {"base_info", {{"channel_version", cfg_.channel_version}}}};

      auto send = fish::capability::HttpClient::instance().post_json(
          base + "/ilink/bot/sendmessage",
          {{"AuthorizationType", "ilink_bot_token"},
           {"Authorization", "Bearer " + auth_headers_token},
           {"X-WECHAT-UIN", random_wechat_uin()}},
          payload.dump(), 20000);
      if (!send.ok()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "wechat",
                              "image_send_failed")
            .field("detail", send.error.detail);
        continue;
      }
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "wechat", "image_sent")
          .field("bytes", static_cast<long long>(plain.size()));
      continue;
    }
    if (block.kind == fish::core::BlockKind::kVoice) {
      // 微信不发语音。
      //
      // 腾讯 iLink 的 bot 外发语音气泡（voice_item type=3）客户端不渲染：
      // 服务端一律 HTTP 200 + message_id，但用户什么都看不到。
      // 降级成文件附件要额外上传一次、用户还得点开，价值不大；
      // 而且云服务器登录微信本身有极高冻结风险，不该在这条链路上加动作。
      // 编排层已按 supports_voice_out=false 把 send_voice 从微信工具列表
      // 过滤掉了，正常不会走到这里——留一条日志便于发现异常调用。
      ::fish::log::log_line(fish::log::LogLevel::kDebug, "wechat", "voice_skipped");
      continue;
    }
  }
  return Error::none();
}

}  // namespace fish::channels