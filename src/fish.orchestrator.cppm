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
import fish.media;
import fish.tool;
import fish.guard;

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
    // 工具注册表：模型可主动调用（网络搜索 / 图片检索 / 记忆检索）
    fish::tool::Registry* tools{nullptr};
    // 单轮最多几次工具调用，防止无限循环
    int max_tool_rounds{3};
    // 安全防护
    fish::guard::InputGuard* input_guard{nullptr};
    fish::guard::OutputGuard* output_guard{nullptr};
    fish::guard::RateLimiter* rate_limiter{nullptr};
    // 媒体缓存：把入站/出站的图片语音落本地副本。
    // 平台给的 URL 是临时签名链接，过期就成死链，记忆里必须存本地路径。
    fish::media::MediaCache* media{nullptr};
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

// 解析回复里的图片标记，**支持多个**。
//   [PHOTO]           → spec 为空（用形象库现成图）
//   [PHOTO: 描述]      → spec 为描述（现场生成）
// 一次回复里可以写多行，按出现顺序全部收集，并把标记从正文里删掉。
//
// 早期只取第一个标记，等于「一次只能发一张」；用户明确要求去掉这个限制，
// 双向都要：出站能发多张，入站能收多张（入站本来就是 vector，不受限）。
//
// 导出是为了可测：这是纯函数，测试直接喂字符串断言结果。
std::vector<std::string> extract_photo_markers(std::string& text);

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

// 解析回复里的图片标记，**支持多个**（声明处有完整说明）。
std::vector<std::string> extract_photo_markers(std::string& text) {
  std::vector<std::string> specs;
  const std::string open = "[PHOTO";

  std::size_t search_from = 0;
  while (true) {
    const auto pos = text.find(open, search_from);
    if (pos == std::string::npos) break;

    const auto close = text.find(']', pos);
    if (close == std::string::npos) break;

    const std::string inner = text.substr(pos + open.size(), close - pos - open.size());
    // inner 形如 "" 或 ": 描述"；不是这两种就说明不是标记，跳过继续找
    if (!inner.empty() && inner[0] != ':') {
      search_from = pos + open.size();
      continue;
    }

    std::string spec;
    if (!inner.empty()) {
      std::string s = inner.substr(1);
      const auto b = s.find_first_not_of(" \t");
      if (b != std::string::npos) spec = s.substr(b);
    }
    specs.push_back(std::move(spec));

    // 删掉标记本身（含其后紧跟的一个换行，避免留下空行）
    std::size_t erase_len = close - pos + 1;
    while (close + erase_len < text.size() &&
           (text[close + erase_len] == '\n' || text[close + erase_len] == ' ')) {
      ++erase_len;
    }
    text.erase(pos, erase_len);
    search_from = pos;   // 删除后原位置继续找，可能有连续标记
  }

  // 清理首尾空行
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
  const auto nb = text.find_first_not_of(" \t\n");
  if (nb != std::string::npos) text = text.substr(nb);
  else text.clear();

  return specs;
}

// 把时间戳转成人类可读的相对/绝对描述，用于提示词。
// 模型不理解 unix 时间戳，但理解"昨天""3 小时前"。
// 超过 7 天用绝对日期，避免"很久以前"这种模糊表述。
std::string format_relative_time(std::int64_t at_ms) {
  if (at_ms <= 0) return {};
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const auto diff_s = (now_ms - at_ms) / 1000;
  if (diff_s < 0) return "刚刚";
  if (diff_s < 60) return "刚刚";
  if (diff_s < 3600) {
    return std::to_string(diff_s / 60) + " 分钟前";
  }
  if (diff_s < 86400) {
    return std::to_string(diff_s / 3600) + " 小时前";
  }
  if (diff_s < 86400 * 7) {
    return std::to_string(diff_s / 86400) + " 天前";
  }
  // 超过一周：给绝对日期
  const std::time_t t = static_cast<std::time_t>(at_ms / 1000);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday);
  return buf;
}

// 解析回复里的语音标记。
//   [VOICE]  → 返回 true，并把标记从文本里删掉
//
// 设计：和 [PHOTO] 一样是显式标记，不是"有 TTS 就每条都发"。
// 语音是重资源（合成耗时、占配额、用户侧还得点开听），
// 应该只在语气需要时用，比如"懒得打字"的场合。
bool extract_voice_marker(std::string& text) {
  const std::string marker = "[VOICE]";
  const auto pos = text.find(marker);
  if (pos == std::string::npos) return false;
  text.erase(pos, marker.size());
  // 清理残留空白
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
      // 平台给的是临时签名 URL，先落本地副本再交给模型。
      // 模型支持直接读 data: URI 或本地文件；更重要的是记忆里能存住。
      fish::core::ContentBlock copy = block;
      if (deps_.media != nullptr) {
        auto cached = deps_.media->fetch(block.media_url, ".jpg");
        if (cached.ok()) {
          copy.media_url = cached.value->path;
          ::fish::log::log_line(fish::log::LogLevel::kDebug, "pipeline", "image_cached")
              .field("path", cached.value->path)
              .field("reused", cached.value->reused);
        } else {
          // 下载失败仍把原始 URL 交给模型：本轮能看就先看
          ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "image_cache_failed")
              .field("detail", cached.error.detail);
        }
      }
      out.push_back(std::move(copy));
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

  // 1.5 安全防护：频率限制 + 输入检查
  if (deps_.rate_limiter != nullptr &&
      !deps_.rate_limiter->allow(msg.conversation_id)) {
    ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "rate_limited")
        .field("conversation", msg.conversation_id);
    return Result<int>::ok(0);   // 静默丢弃，不给刷屏者反馈
  }
  if (deps_.input_guard != nullptr) {
    const auto verdict = deps_.input_guard->check(msg);
    if (!verdict.allow) {
      ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "input_rejected")
          .field("rule", verdict.rule)
          .field("conversation", msg.conversation_id);
      // 有 note 就回一句；没有就静默
      if (!verdict.note.empty()) {
        fish::core::OutboundMessage reply;
        reply.conversation_id = msg.conversation_id;
        reply.reply_to_message_id = msg.message_id;
        reply.context_token = msg.context_token;
        fish::core::ContentBlock b;
        b.kind = fish::core::BlockKind::kText;
        b.text = verdict.note;
        reply.blocks.push_back(std::move(b));
        channel.send(reply, 1);
      }
      return Result<int>::ok(0);
    }
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
  // 落盘用的文本要标出「对方发了图」。
  // 图片已由媒体缓存落成本地副本，路径存进 Message::image_paths，
  // 下一轮组装历史时会重新作为多模态内容带进去，模型能一直看到。
  std::string remembered_query = query;
  std::vector<std::string> inbound_images;
  for (const auto& b : *input.value) {
    if (b.kind == fish::core::BlockKind::kImage && !b.media_url.empty()) {
      inbound_images.push_back(b.media_url);
    }
  }
  if (!inbound_images.empty()) {
    if (remembered_query == "[图片]") {
      remembered_query = "[对方发来一张图片]";
    } else {
      remembered_query += " [对方发来一张图片]";
    }
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

    // 发言人 + 时间：群聊里必须让模型知道「是谁、什么时候说的」，
    // 否则多人对话会糊成一片，也理解不了"昨天""刚才"这类指代。
    if (b.role == "user") {
      b.speaker = m.speaker;
      b.at_ms = m.at_ms;
      const std::string who = m.speaker.empty() ? std::string{} : m.speaker.display_name();
      const std::string when = format_relative_time(m.at_ms);
      if (!who.empty() || !when.empty()) {
        std::string tag;
        if (!who.empty()) tag += who;
        if (!when.empty()) {
          if (!tag.empty()) tag += "，";
          tag += when;
        }
        b.text = "[" + tag + "] " + b.text;
      }
    }
    history.push_back(std::move(b));

    // 图片按发送者归属回传，role 标明是谁发的。
    // 具体怎么编码成请求（assistant 图是否降级成单独的 user 消息 + 归属标签）
    // 由 fish.capability 根据上游能力决定，这里只如实传递归属。
    const std::string img_role = (m.role == "user") ? "user" : "assistant";
    for (const auto& path : m.image_paths) {
      if (path.empty()) continue;
      fish::core::ContentBlock img;
      img.kind = fish::core::BlockKind::kImage;
      img.media_url = path;
      img.mime = "image/png";
      img.role = img_role;
      img.speaker = m.speaker;
      history.push_back(std::move(img));
    }
  }

  // 9. 模型调用（带降级 + 工具循环）
  fish::core::IChatModel::Request req;
  // system 只放稳定前缀：它必须逐字节不变，否则前缀缓存全失效（成本差 50 倍）。
  req.system_prompt = deps_.persona->stable_prefix();

  req.user_blocks = std::move(*input.value);
  req.history = std::move(history);

  // 动态后缀：本轮上下文（时间/场景/对方）+ 本轮红线。
  //
  // 这段原来只定义了、从未调用，等于模型不知道现在几点、是群聊还是私聊，
  // 末尾那句红线也从未生效。
  //
  // 放进 system 会污染前缀缓存，所以作为 history 的**最后一条 user 消息**注入：
  // 它跟着本轮输入走，不改变前面的稳定前缀。
  //
  // 注意：dynamic_suffix 还预留了【相关记忆】参数，但 MemoryContext 目前
  // 没有召回条目字段，传空串——不编造不存在的数据。
  const std::string suffix = deps_.persona->dynamic_suffix(msg, "");
  if (!suffix.empty()) {
    fish::core::ContentBlock ctx_block;
    ctx_block.kind = fish::core::BlockKind::kText;
    ctx_block.text = suffix;
    ctx_block.role = "user";
    req.user_blocks.insert(req.user_blocks.begin(), std::move(ctx_block));
  }

  // 工具声明：只在注册表非空时带上
  if (deps_.tools != nullptr && !deps_.tools->empty()) {
    req.tools_json = deps_.tools->openai_tools_json();
  }

  auto resp = deps_.chat->complete(req);

  // 工具循环：模型要求调用工具 → 执行 → 把结果作为 user 消息追加 → 再问
  // 最多 max_tool_rounds 轮，防止模型陷入死循环。
  int tool_round = 0;
  while (resp.ok() && resp.value->wants_tools() &&
         deps_.tools != nullptr && tool_round < deps_.max_tool_rounds) {
    ++tool_round;

    // 把「模型想调用什么」和「工具返回了什么」拼成下一轮的上下文。
    // 用 user 角色承载工具结果：上游对 tool 角色的支持不稳定，
    // 实测用 user + 明确标注更可靠。
    std::string tool_block;
    for (const auto& call : resp.value->tool_calls) {
      fish::tool::Call tc;
      tc.name = call.name;
      tc.arguments_json = call.arguments;

      auto outcome = deps_.tools->run(tc);
      ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline", "tool_ran")
          .field("tool", call.name)
          .field("ok", outcome.ok)
          .field("round", static_cast<long long>(tool_round));

      tool_block += "【工具调用】" + call.name + "\n";
      tool_block += "【返回】" + outcome.content + "\n\n";
    }

    if (tool_block.empty()) break;

    // 追加到本轮输入，让模型基于工具结果作答
    fish::core::ContentBlock tb;
    tb.kind = fish::core::BlockKind::kText;
    tb.text = "[系统：以下是工具调用的真实结果，请据此回答，不要编造]\n" + tool_block;
    tb.role = "user";
    req.user_blocks.push_back(std::move(tb));

    // 清空 tools，避免模型反复要求调用同一工具
    // （第二轮通常应该直接给答案）
    resp = deps_.chat->complete(req);
  }

  std::string reply_text;
  if (resp.ok()) {
    reply_text = resp.value->text;
    // 工具调用后模型可能只返回空文本：给个兜底
    if (reply_text.empty() && tool_round > 0) {
      reply_text = "查了一下，没找到特别确定的说法。";
    }
  } else {
    reply_text = "我卡了一下…你刚才说什么？";
    ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "chat_degraded")
        .field("detail", resp.error.detail);
  }

  // 9.8 输出防护：脱敏 + 去链接 + 长度截断
  // 放在解析 [PHOTO] 之前：链接会被 QQ 拦截，先清掉再判断图片标记。
  if (deps_.output_guard != nullptr) {
    reply_text = deps_.output_guard->sanitize(reply_text);
  }

  // 9.5 解析图片标记：[PHOTO] 取库存图，[PHOTO: 描述] 现场生成
  // 支持多个标记，按出现顺序逐张发送。
  const std::vector<std::string> photo_specs = extract_photo_markers(reply_text);
  const bool wants_photo = !photo_specs.empty();

  // 记忆里要记「实际发出去的东西」，而不是模型原始输出。
  // 模型输出 "[PHOTO]" 时，用户实际看到的是「喏」+ 一张图；
  // 若把剥离后的空 reply_text 存进去，下一轮模型就不知道自己发过图，
  // 会说出「我看不到自己发出去的东西」这种话。
  //
  // 标记格式必须让模型**无法当成台词模仿**：
  // 早期用 "[发了一张自己的照片]"，结果模型把它当输出格式照抄，
  // 在正文里写出「再试一次。[发了一张自己的照片]」这种动作描写。
  // 改成「（附了一张图）」这种圆括号旁注 + 明确「这不是台词」的措辞。
  std::string remembered_reply = reply_text;
  if (wants_photo) {
    if (remembered_reply.empty()) remembered_reply = "喏";
    // 多张时逐张记，别把 N 张压成一句
    for (const auto& spec : photo_specs) {
      remembered_reply += spec.empty()
                              ? "（系统附了一张图）"
                              : "（系统附了一张图：" + spec + "）";
    }
  }

  // 出站图片的本地路径，稍后一起写进记忆。
  // 必须存路径而不是只存文字：模型被问「描述一下这张图」时
  // 需要真的把图重新带进上下文，否则只能回答「我看不到」。
  std::vector<std::string> outbound_images;

  // 10. 落盘 + 巩固（推迟到发图之后，见下方：需要带上图片路径）


  // 11-12. 组装出站，按预算逐条发送
  std::vector<fish::core::ContentBlock> outgoing;
  fish::core::ContentBlock text_block;
  text_block.kind = fish::core::BlockKind::kText;
  text_block.text = reply_text;
  // 只有图片、没有文字时，发一个极短的话兜底（空文本会被通道丢弃）
  if (text_block.text.empty() && wants_photo) text_block.text = "喏";
  if (!text_block.text.empty()) outgoing.push_back(std::move(text_block));

  // 图片：先查形象库（零成本、形象稳定），没有再现场生成。
  // 支持多张：每个 [PHOTO] 标记对应一张，按顺序发送。
  for (const auto& photo_spec : photo_specs) {
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
      img.media_url = photo_url;
      img.mime = "image/png";
      img.role = "assistant";
      outbound_images.push_back(photo_url);   // 记忆里要存路径
      outgoing.push_back(std::move(img));
      ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline", "photo_attached")
          .field("source", photo_src);
    }
  }

  // 语音：只在模型主动要求时发，不是每条都发。
  //
  // 之前是「只要 TTS 可用就每条都合成语音」，结果每条消息都带一段语音，
  // 既吵又浪费配额。改成和图片一样的标记机制：模型用 [VOICE] 表示
  // "这条我想用语音说"，通常用于语气需要表达的场合。
  const bool wants_voice = extract_voice_marker(reply_text);
  const bool voice_ok = wants_voice && deps_.tts != nullptr &&
                        cap.supports_voice_out && !wants_photo &&
                        (msg.kind == fish::core::ChatKind::kPrivate || deps_.group_voice);
  if (voice_ok) {
    auto audio = deps_.tts->synthesize(reply_text);
    if (audio.ok()) {
      fish::core::ContentBlock voice;
      voice.kind = fish::core::BlockKind::kVoice;
      voice.media_url = std::move(*audio.value);
      voice.mime = "audio/mpeg";
      outgoing.push_back(std::move(voice));
      ::fish::log::log_line(fish::log::LogLevel::kInfo, "pipeline", "voice_attached");
    } else {
      ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "tts_failed")
          .field("detail", audio.error.detail);
    }
  }

  // 出站条数上限。
  //
  // 注意：这是**平台被动回复配额**（QQ 官方单聊 4 / 群 5），
  // 不是我们自己设的「一次只能发一张图」。
  // 图片按张各占一条，所以多图会吃掉配额；配额耗尽时后面的发不出去，
  // 这属于平台硬约束，日志里会显式记 quota_truncated，不做静默截断。
  const int allow = cap.max_replies_per_message;
  if (allow > 0 && static_cast<int>(outgoing.size()) > allow) {
    ::fish::log::log_line(fish::log::LogLevel::kWarn, "pipeline", "quota_truncated")
        .field("wanted", static_cast<long long>(outgoing.size()))
        .field("allow", static_cast<long long>(allow))
        .field("dropped", static_cast<long long>(outgoing.size() - allow));
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

  // 10. 落盘 + 巩固
  // 放在发送之后：这样能把「实际发出去的图片路径」一并写进记忆，
  // 下一轮模型重新拿到这些图，才能真正回答「描述一下这张图」。
  if (deps_.memory != nullptr) {
    fish::memory::Message user_msg;
    user_msg.role = "user";
    user_msg.text = remembered_query;
    user_msg.at = msg.received_at;
    user_msg.image_paths = std::move(inbound_images);
    // 发言人 + 时间：群聊里必须记，否则模型分不清是群里哪个人说的
    user_msg.speaker = msg.speaker();
    user_msg.at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         msg.received_at.time_since_epoch())
                         .count();

    fish::memory::Message bot_msg;
    bot_msg.role = "assistant";
    bot_msg.text = remembered_reply;
    bot_msg.at = fish::core::Clock::now();
    bot_msg.image_paths = std::move(outbound_images);
    bot_msg.at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        bot_msg.at.time_since_epoch())
                        .count();

    deps_.memory->record(msg.conversation_id, user_msg, bot_msg);
    // 记忆压缩已在 gather 前由 needs_compaction()/compact() 处理
  }

  return Result<int>::ok(sent);
}

}  // namespace fish::orchestrator
