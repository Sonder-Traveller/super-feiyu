// src/fish.orchestrator.cppm
// 编排层：唯一持有业务决策（要不要回、回什么、怎么回）。
module;
#include <algorithm>
#include <array>
#include <functional>
#include <chrono>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

export module fish.orchestrator;

import fish.core;
import fish.log;
import fish.memory;
import fish.image;

export namespace fish::orchestrator {

// 被动对话的准入判定
class ReplyPolicy {
 public:
  struct Decision {
    bool should_reply{false};
    std::string reason;
  };
  [[nodiscard]] Decision evaluate(const fish::core::InboundMessage& msg,
                                  const fish::core::ChannelCapability& cap) const;
};

// 去重：平台重连/轮询超时会重发事件
class DedupCache {
 public:
  explicit DedupCache(std::size_t capacity = 1024);
  bool seen(const std::string& key);
  std::size_t sweep();

 private:
  struct Entry {
    fish::core::TimePoint at{};
  };
  std::size_t capacity_;
  std::mutex mu_;
  std::unordered_map<std::string, Entry> seen_;
};

// 回复预算：QQ 官方限制单聊 4 条 / 群 5 条
class ReplyBudget final : public fish::core::IReplyBudget {
 public:
  explicit ReplyBudget(int limit_per_message,
                       std::chrono::seconds window = std::chrono::minutes(70));
  [[nodiscard]] int remaining(const std::string& message_id) const override;
  bool consume(const std::string& message_id) override;
  std::size_t sweep();

 private:
  struct Entry {
    int used{0};
    fish::core::TimePoint expires_at{};
  };
  int limit_;
  std::chrono::seconds window_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> entries_;
};

// 分层人格：稳定前缀（命中缓存）+ 动态尾部
class LayeredPersona final : public fish::core::IPersona {
 public:
  LayeredPersona(std::string persona_text, std::string constraints);
  [[nodiscard]] std::string stable_prefix() const override;
  [[nodiscard]] std::string dynamic_suffix(const fish::core::InboundMessage& msg,
                                           const std::string& recalled_memory) const override;

 private:
  std::string prefix_;
};

fish::core::Result<LayeredPersona> load_persona(const std::string& persona_path,
                                                const std::string& constraints_path);

// 主链路
class Pipeline {
 public:
  struct Deps {
    fish::core::IChatModel* chat{nullptr};
    fish::core::IEmbeddingModel* embedding{nullptr};
    fish::core::IPersona* persona{nullptr};
    fish::core::IReplyBudget* budget{nullptr};
    fish::core::ITextToSpeech* tts{nullptr};
    fish::core::ISpeechToText* stt{nullptr};
    fish::core::IImageModel* image{nullptr};      // 生图（可为空）
    fish::image::PortraitStore* portraits{nullptr};  // 形象库（可为空）
    fish::memory::MemorySystem* memory{nullptr};
    fish::memory::Consolidator* consolidator{nullptr};
    bool group_voice{false};
    int passive_window_ms{300000};
  };

  explicit Pipeline(Deps deps);
  fish::core::Result<int> handle(fish::core::IChannel& channel,
                                 const fish::core::InboundMessage& msg);
  DedupCache& dedup() { return dedup_; }

  // 周期维护：清理预算/去重表。由 runtime 的定时线程调用。
  void maintenance();

 private:
  fish::core::Result<std::vector<fish::core::ContentBlock>> normalize_input(
      const fish::core::InboundMessage& msg);

  // 同一会话必须串行：否则两条消息并发处理会导致
  // 回复顺序错乱、记忆写入竞争、上下文不一致。
  // 用固定数量的条带锁（而非每会话一把），内存恒定。
  [[nodiscard]] std::mutex& lock_for(const std::string& conv_id) const;

  Deps deps_;
  ReplyPolicy policy_;
  DedupCache dedup_;
  static constexpr std::size_t kShards = 64;
  mutable std::array<std::mutex, kShards> shards_{};
};

}  // namespace fish::orchestrator

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::orchestrator {

using fish::core::ErrorCode;
using fish::core::Result;

// ── ReplyPolicy ─────────────────────────────────────────
ReplyPolicy::Decision ReplyPolicy::evaluate(const fish::core::InboundMessage& msg,
                                            const fish::core::ChannelCapability& cap) const {
  if (msg.kind == fish::core::ChatKind::kPrivate) {
    if (!cap.supports_private) return {false, "private_unsupported"};
    return {true, "private"};
  }
  // 群聊：被动红线——未 @ 一律不回
  if (!cap.supports_group) return {false, "group_unsupported"};
  if (!msg.mentioned_me) return {false, "group_not_mentioned"};
  return {true, "group_mentioned"};
}

// ── DedupCache ──────────────────────────────────────────
DedupCache::DedupCache(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

bool DedupCache::seen(const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = seen_.find(key);
  if (it != seen_.end()) {
    it->second.at = fish::core::Clock::now();
    return true;
  }
  if (seen_.size() >= capacity_) {
    auto oldest = seen_.begin();
    for (auto i = seen_.begin(); i != seen_.end(); ++i) {
      if (i->second.at < oldest->second.at) oldest = i;
    }
    seen_.erase(oldest);
  }
  seen_[key] = Entry{fish::core::Clock::now()};
  return false;
}

std::size_t DedupCache::sweep() {
  std::lock_guard<std::mutex> lock(mu_);
  const auto cutoff = fish::core::Clock::now() - std::chrono::minutes(5);
  std::size_t removed = 0;
  for (auto it = seen_.begin(); it != seen_.end();) {
    if (it->second.at < cutoff) {
      it = seen_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

// ── ReplyBudget ─────────────────────────────────────────
ReplyBudget::ReplyBudget(int limit_per_message, std::chrono::seconds window)
    : limit_(limit_per_message), window_(window) {}

int ReplyBudget::remaining(const std::string& message_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = entries_.find(message_id);
  if (it == entries_.end()) return limit_;
  if (fish::core::Clock::now() >= it->second.expires_at) return limit_;
  return limit_ - it->second.used;
}

bool ReplyBudget::consume(const std::string& message_id) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = fish::core::Clock::now();
  auto it = entries_.find(message_id);
  if (it == entries_.end() || now >= it->second.expires_at) {
    entries_[message_id] = Entry{1, now + window_};
    return true;
  }
  if (it->second.used >= limit_) return false;
  ++it->second.used;
  return true;
}

std::size_t ReplyBudget::sweep() {
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = fish::core::Clock::now();
  std::size_t removed = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (now >= it->second.expires_at) {
      it = entries_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

// ── LayeredPersona ──────────────────────────────────────
LayeredPersona::LayeredPersona(std::string persona_text, std::string constraints) {
  // 顺序固定：约束在前、人格在后。顺序一变历史缓存全失效
  std::ostringstream oss;
  oss << constraints << "\n\n" << persona_text;
  prefix_ = oss.str();
}

std::string LayeredPersona::stable_prefix() const { return prefix_; }

std::string LayeredPersona::dynamic_suffix(const fish::core::InboundMessage& msg,
                                           const std::string& recalled_memory) const {
  std::ostringstream oss;

  const auto tp = msg.received_at.time_since_epoch().count() != 0 ? msg.received_at
                                                                  : fish::core::Clock::now();
  const auto t = fish::core::Clock::to_time_t(tp);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H时", &tm_buf);

  oss << "【本轮上下文】\n";
  oss << "现在时间：" << buf << "\n";
  oss << "场景：" << (msg.kind == fish::core::ChatKind::kGroup ? "群聊" : "私聊") << "\n";
  if (!msg.sender_name.empty()) oss << "对方：" << msg.sender_name << "\n";

  if (!recalled_memory.empty()) oss << "\n【相关记忆】\n" << recalled_memory << "\n";

  // 红线放最后：近因效应，执行力最强
  oss << "\n【本轮红线】只回这一条，不要主动开启新话题；不超过两句；不要暴露你是 AI。";
  return oss.str();
}

Result<LayeredPersona> load_persona(const std::string& persona_path,
                                    const std::string& constraints_path) {
  auto read_file = [](const std::string& path) -> Result<std::string> {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Result<std::string>::fail(ErrorCode::kBadRequest, "cannot_open:" + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    auto text = ss.str();
    if (text.empty()) {
      return Result<std::string>::fail(ErrorCode::kBadRequest, "empty_file:" + path);
    }
    return Result<std::string>::ok(std::move(text));
  };

  auto p = read_file(persona_path);
  if (!p.ok()) return Result<LayeredPersona>::fail(p.error.code, p.error.detail);
  auto c = read_file(constraints_path);
  if (!c.ok()) return Result<LayeredPersona>::fail(c.error.code, c.error.detail);

  return Result<LayeredPersona>::ok(
      LayeredPersona(std::move(*p.value), std::move(*c.value)));
}

// ── Pipeline ────────────────────────────────────────────
Pipeline::Pipeline(Deps deps) : deps_(std::move(deps)) {}

std::mutex& Pipeline::lock_for(const std::string& conv_id) const {
  const std::size_t h = std::hash<std::string>{}(conv_id);
  return shards_[h % kShards];
}

void Pipeline::maintenance() {
  const auto dropped = dedup_.sweep();
  if (dropped > 0) {
    ::fish::log::log_line(::fish::log::LogLevel::kDebug, "pipeline", "dedup_swept")
        .field("dropped", static_cast<long long>(dropped));
  }
}

// 解析回复里的图片标记。
//   [PHOTO]          → 返回 true，spec 为空（用形象库现成图）
//   [PHOTO: 描述]     → 返回 true，spec 为描述（现场生成）
// 同时把标记从 reply_text 里删掉，避免被当成正文发出去。
bool extract_photo_marker(std::string& text, std::string* spec) {
  const std::string open = "[PHOTO";
  const auto pos = text.find(open);
  if (pos == std::string::npos) return false;

  const auto close = text.find(']', pos);
  if (close == std::string::npos) return false;

  const std::string inner = text.substr(pos + open.size(), close - pos - open.size());
  // inner 形如 "" 或 ": 描述"
  if (!inner.empty()) {
    if (inner[0] != ':') return false;
    std::string s = inner.substr(1);
    const auto b = s.find_first_not_of(" \t");
    if (b != std::string::npos) s = s.substr(b);
    *spec = s;
  }

  // 删掉标记本身，并清理残留空行
  text.erase(pos, close - pos + 1);
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
  const auto nb = text.find_first_not_of(" \t\n");
  if (nb != std::string::npos) text = text.substr(nb);
  return true;
}

Result<std::vector<fish::core::ContentBlock>> Pipeline::normalize_input(
    const fish::core::InboundMessage& msg) {
  std::vector<fish::core::ContentBlock> out;
  out.reserve(msg.blocks.size());

  for (const auto& block : msg.blocks) {
    if (block.kind == fish::core::BlockKind::kText) {
      if (!block.text.empty()) out.push_back(block);
      continue;
    }
    if (block.kind == fish::core::BlockKind::kImage) {
      out.push_back(block);  // 图片透传给多模态模型
      continue;
    }
    if (block.kind == fish::core::BlockKind::kVoice) {
      fish::core::ContentBlock note;
      note.kind = fish::core::BlockKind::kText;
      if (deps_.stt == nullptr) {
        note.text = "[语音消息，未启用转写]";
      } else {
        auto stt = deps_.stt->transcribe(block.media_url);
        if (stt.ok()) {
          note.text = std::move(*stt.value);
        } else {
          note.text = "[语音消息转写失败]";
          ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "stt_failed")
              .field("detail", stt.error.detail);
        }
      }
      out.push_back(std::move(note));
    }
  }

  if (out.empty()) {
    return Result<std::vector<fish::core::ContentBlock>>::fail(ErrorCode::kBadRequest,
                                                               "no_usable_block");
  }
  return Result<std::vector<fish::core::ContentBlock>>::ok(std::move(out));
}

Result<int> Pipeline::handle(fish::core::IChannel& channel,
                             const fish::core::InboundMessage& msg) {
  const auto cap = channel.capability();

  // 1. 去重
  if (dedup_.seen(msg.message_id)) {
    ::fish::log::log_line(fish::log::LogLevel::kDebug, "pipeline", "duplicate_dropped")
        .field("msg_id", msg.message_id);
    return Result<int>::ok(0);
  }

  // 2. 时效检查：超过被动窗口 80% 放弃
  if (deps_.passive_window_ms > 0 && msg.received_at.time_since_epoch().count() != 0) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             fish::core::Clock::now() - msg.received_at)
                             .count();
    if (elapsed > static_cast<long long>(deps_.passive_window_ms) * 8 / 10) {
      ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline", "expired_dropped")
          .field("msg_id", msg.message_id)
          .field("elapsed_ms", static_cast<long long>(elapsed));
      return Result<int>::ok(0);
    }
  }

  // 3. 同会话串行化：拿条带锁，避免并发处理同会话消息
  std::lock_guard<std::mutex> conv_lock(lock_for(msg.conversation_id));

  // 4. 准入判定
  const auto decision = policy_.evaluate(msg, cap);
  if (!decision.should_reply) {
    ::fish::log::log_line(fish::log::LogLevel::kDebug, "pipeline", "silent")
        .field("reason", decision.reason);
    return Result<int>::ok(0);
  }

  // 5. 预算
  if (deps_.budget != nullptr && deps_.budget->remaining(msg.message_id) <= 0) {
    ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "budget_exhausted")
        .field("msg_id", msg.message_id);
    return Result<int>::fail(ErrorCode::kBudgetExhausted, "reply_budget_exhausted");
  }

  // 6. 多模态归一化
  auto input = normalize_input(msg);
  if (!input.ok()) return Result<int>::fail(input.error.code, input.error.detail);

  std::string query;
  int image_count = 0;
  for (const auto& b : *input.value) {
    if (b.kind == fish::core::BlockKind::kText) {
      if (!query.empty()) query += " ";
      query += b.text;
    } else if (b.kind == fish::core::BlockKind::kImage) {
      ++image_count;
    }
  }
  // 纯图片消息：query 为空会让向量召回/落盘拿到空串，补一个可检索的占位
  if (query.empty() && image_count > 0) {
    query = "[图片]";
  }

  // 7. 记忆召回
  // 7. 若原始对话超出 token 预算 → 把最早的压缩成摘要 + 落盘
  //    触发条件是「超预算」，不是轮数。
  if (deps_.memory != nullptr && deps_.consolidator != nullptr) {
    if (deps_.memory->needs_compaction(msg.conversation_id)) {
      auto c = deps_.memory->compact(msg.conversation_id, *deps_.consolidator);
      if (!c.ok()) {
        ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "compact_failed")
            .field("detail", c.error.detail);
      }
    }
  }

  fish::memory::MemoryContext mem_ctx;
  if (deps_.memory != nullptr) {
    auto g = deps_.memory->gather(msg.conversation_id, query);
    if (g.ok()) mem_ctx = std::move(*g.value);
  }

  // 8. 提示词分层：摘要 + 预算内全量原文
  std::vector<fish::core::ContentBlock> history;
  if (!mem_ctx.summary.empty()) {
    fish::core::ContentBlock b;
    b.kind = fish::core::BlockKind::kText;
    b.text = "【更早的对话摘要】\n" + mem_ctx.summary;
    b.role = "user";
    history.push_back(std::move(b));
  }
  for (const auto& m : mem_ctx.working) {
    fish::core::ContentBlock b;
    b.kind = fish::core::BlockKind::kText;
    b.text = m.text;
    // 角色必须显式传递：模型据此区分谁说的
    b.role = (m.role == "user") ? "user" : "assistant";
    history.push_back(std::move(b));
  }

  // 9. 模型调用（带降级）
  fish::core::IChatModel::Request req;
  req.system_prompt = deps_.persona->stable_prefix();
  req.user_blocks = std::move(*input.value);
  req.history = std::move(history);

  auto resp = deps_.chat->complete(req);
  std::string reply_text;
  if (resp.ok()) {
    reply_text = resp.value->text;
  } else {
    reply_text = "我卡了一下…你刚才说什么？";
    ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "chat_degraded")
        .field("detail", resp.error.detail);
  }

  // 9.5 解析图片标记：[PHOTO] 取库存图，[PHOTO: 描述] 现场生成
  std::string photo_spec;
  const bool wants_photo = extract_photo_marker(reply_text, &photo_spec);

  // 10. 落盘 + 巩固
  if (deps_.memory != nullptr) {
    fish::memory::Message user_msg;
    user_msg.role = "user";
    user_msg.text = query;
    user_msg.at = msg.received_at;
    fish::memory::Message bot_msg;
    bot_msg.role = "assistant";
    bot_msg.text = reply_text;
    bot_msg.at = fish::core::Clock::now();
    deps_.memory->record(msg.conversation_id, user_msg, bot_msg);

    // 记忆压缩已在 gather 前由 needs_compaction()/compact() 处理
  }

  // 11-12. 组装出站，按预算逐条发送
  std::vector<fish::core::ContentBlock> outgoing;
  fish::core::ContentBlock text_block;
  text_block.kind = fish::core::BlockKind::kText;
  text_block.text = reply_text;
  // 只有图片、没有文字时，发一个极短的话兜底（空文本会被通道丢弃）
  if (text_block.text.empty() && wants_photo) text_block.text = "喏";
  if (!text_block.text.empty()) outgoing.push_back(std::move(text_block));

  // 图片：先查形象库（零成本、形象稳定），没有再现场生成
  if (wants_photo) {
    std::string photo_url;
    std::string photo_src;

    if (photo_spec.empty() && deps_.portraits != nullptr) {
      const auto p = deps_.portraits->pick(query, fish::image::PickStrategy::kTagMatch);
      if (!p.path.empty()) {
        photo_url = p.path;
        photo_src = "library";
      }
    }

    if (photo_url.empty() && deps_.image != nullptr) {
      fish::core::IImageModel::Request ireq;
      ireq.prompt = photo_spec.empty()
                        ? "一只蓝色的鲸鱼娘，日常抓拍，自然光线，手机随手拍的质感"
                        : photo_spec;
      auto gen = deps_.image->generate(ireq);
      if (gen.ok()) {
        photo_url = gen.value->image_url;
        photo_src = "generated";
        // 生成的图也进库，下次同类场景直接复用
        if (deps_.portraits != nullptr && !photo_url.empty()) {
          fish::image::Portrait np;
          np.path = photo_url;
          np.caption = photo_spec;
          np.source = "generated";
          deps_.portraits->add(np);
        }
      } else {
        ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "image_failed")
            .field("detail", gen.error.detail);
      }
    }

    if (!photo_url.empty()) {
      // 本地路径直接交给通道：QQ 官方支持二进制直传（file_data），
      // 不需要公网 URL，也不需要开放任何端口。
      fish::core::ContentBlock img;
      img.kind = fish::core::BlockKind::kImage;
      img.media_url = std::move(photo_url);
      img.mime = "image/png";
      outgoing.push_back(std::move(img));
      ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline", "photo_attached")
          .field("source", photo_src);
    }
  }

  const bool voice_ok = deps_.tts != nullptr && cap.supports_voice_out &&
                        (msg.kind == fish::core::ChatKind::kPrivate || deps_.group_voice);
  if (voice_ok && !wants_photo) {   // 发图时不叠加语音，避免一次回太多
    auto audio = deps_.tts->synthesize(reply_text);
    if (audio.ok()) {
      fish::core::ContentBlock voice;
      voice.kind = fish::core::BlockKind::kVoice;
      voice.media_url = std::move(*audio.value);
      voice.mime = "audio/mpeg";
      outgoing.push_back(std::move(voice));
    }
  }

  const int allow = cap.max_replies_per_message;
  if (static_cast<int>(outgoing.size()) > allow) {
    outgoing.resize(static_cast<std::size_t>(allow));
  }

  int sent = 0;
  for (int i = 0; i < static_cast<int>(outgoing.size()); ++i) {
    if (deps_.budget != nullptr && !deps_.budget->consume(msg.message_id)) break;

    fish::core::OutboundMessage one;
    one.conversation_id = msg.conversation_id;
    one.reply_to_message_id = msg.message_id;
    one.context_token = msg.context_token;
    one.blocks.push_back(outgoing[static_cast<std::size_t>(i)]);

    auto err = channel.send(one, i + 1);
    if (!err.ok()) {
      ::fish::log::log_line(fish::log::LogLevel::kError, "pipeline", "send_failed")
          .field("conversation", msg.conversation_id)
          .field("detail", err.detail);
      return Result<int>::fail(err.code, err.detail);
    }
    ++sent;
  }

  ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline", "replied")
      .field("conversation", msg.conversation_id)
      .field("chunks", sent)
      .field("chars", static_cast<long long>(reply_text.size()));

  return Result<int>::ok(sent);
}

}  // namespace fish::orchestrator
