// src/fish.session.cppm
// 会话上下文：把「一个会话」的运行时状态打包，跟着任务在线程池里走。
//
// 为什么需要：
//   线程池是无状态的，worker 只负责执行任务。但处理一条消息需要
//   一整套会话相关的东西：记忆、去重、被动回复窗口、串行化。
//   如果这些靠全局查表，任务在不同 worker 间迁移时容易串味，
//   也容易忘记隔离。
//
//   所以把会话相关的一切封进 SessionContext，随任务一起传递。
//   任务在哪个 worker 上跑都无所谓，它拿到的 ctx 已经限定了会话。
//
// 隔离要点：
//   - 每个会话一个 key（群聊 g:<openid>，私聊 <openid>）
//     **约定：只有 session_key() 加 "g:" 前缀**。渠道层（qq_ws / onebot_ws /
//     channels）的 InboundMessage::conversation_id 只放原始 openid。
//     否则群消息会拼成 "g:g:<openid>"，与记忆/人格用的 "g:<openid>" 错位。
//   - 每个会话一把锁：同会话串行，不同会话并行
//   - 记忆落盘按 id 分目录，互不可见
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

export module fish.session;

import fish.core;
import fish.log;

export namespace fish::session {

// 会话键：从入站消息推导，全项目统一用这一个函数，
// 避免各处自己拼字符串导致隔离失效。
//
// 这是**唯一**加 "g:" 前缀的地方：渠道层的 conversation_id 只放原始
// group_openid / group_id，前缀在这里统一加。
//
// 兼容性：历史上渠道层也加过一次前缀，导致群会话键是 "g:g:<openid>"，
// 而编排层用裸 conversation_id（"g:<openid>"）查记忆和人格。改成单前缀后：
//   - 记忆键没变（编排层原本就是 "g:<openid>"），旧数据继续可用；
//   - 人格覆盖键从 "g:g:<openid>" 变回 "g:<openid>"，data/persona/sessions/
//     里按旧键落盘的文件不会再被命中，需要在 GUI 重新保存一次人格。
// 不做数据迁移。
[[nodiscard]] inline std::string session_key(const fish::core::InboundMessage& msg) {
  // 群聊必须带 g: 前缀，否则群 openid 和用户 openid 可能撞车
  if (msg.kind == fish::core::ChatKind::kGroup) {
    return "g:" + msg.conversation_id;
  }
  return msg.conversation_id;
}

// 渠道名（登记表用）。core 不带 to_string 是刻意的——渠道实现各有自己的
// 标识；这里只需要日志/展示级的名字。
[[nodiscard]] inline std::string channel_name(fish::core::Channel c) {
  return c == fish::core::Channel::kWeChat ? "wechat" : "qq";
}

// 会话元信息：只放不可变的、便于日志和调试的部分
struct SessionInfo {
  std::string key;          // session_key() 的结果
  std::string channel;      // "qq_official" / "qq_onebot" / "wechat" / "local"
  fish::core::ChatKind kind{fish::core::ChatKind::kPrivate};
  std::uint64_t created_at_ms{0};
  std::uint64_t last_active_ms{0};
  std::uint64_t handled{0};   // 该会话累计处理的消息数
};

// 单会话上下文。任务执行期间持有，保证同会话串行。
class SessionContext {
 public:
  explicit SessionContext(SessionInfo info) : info_(std::move(info)) {}

  [[nodiscard]] const std::string& key() const noexcept { return info_.key; }
  [[nodiscard]] const SessionInfo& info() const noexcept { return info_; }
  [[nodiscard]] SessionInfo& info() noexcept { return info_; }

  // 同会话串行：不同会话互不阻塞
  [[nodiscard]] std::mutex& lock() noexcept { return mu_; }

 private:
  SessionInfo info_;
  std::mutex mu_;
};

using SessionPtr = std::shared_ptr<SessionContext>;

// 会话注册表：key → SessionContext。
// 只负责「找到或创建」，不做淘汰（会话是资产，不按时间清理）。
class SessionRegistry {
 public:
  // 取或建。线程安全。
  SessionPtr get_or_create(const SessionInfo& info) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(info.key);
    if (it != map_.end()) {
      it->second->info().last_active_ms = info.last_active_ms;
      return it->second;
    }
    auto ctx = std::make_shared<SessionContext>(info);
    map_.emplace(info.key, ctx);
    return ctx;
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.size();
  }

  // 快照（供 metrics 打印）
  [[nodiscard]] std::vector<SessionInfo> snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SessionInfo> out;
    out.reserve(map_.size());
    for (const auto& [k, v] : map_) out.push_back(v->info());
    return out;
  }

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, SessionPtr> map_;
};

// ── 会话登记表 ──────────────────────────────────────────
//
// GUI 人格页要展示「已有的 bot/群」卡片（头像 + 名称 + 唯一标识小字），
// 所以每个见过的会话都登记一份元信息，落盘到 data/conversations.json。
//
// 为什么不放在 SessionRegistry：那是运行时状态（锁、上下文），
// 重启即空；登记表是持久资产，服务重启后 GUI 还要能列出来。
class ConversationBook {
 public:
  struct Entry {
    std::string key;             // session_key() 的结果，唯一标识
    std::string channel;         // qq_official / wechat / ...
    std::string kind;            // group / private
    std::string title;           // 展示名：群名或对方昵称（会变，仅展示）
    std::string sender_name;     // 最近一次发言的人
    std::string sender_id;       // 对方的稳定标识（openid / QQ 号）
    std::uint64_t last_active_ms{0};
    std::uint64_t handled{0};
  };

  explicit ConversationBook(std::string path) : path_(std::move(path)) {
    // 目录必须在这里建。
    //
    // 之前不建目录 → ofstream 打不开 → save_locked 静默失败 →
    // 登记表永远不落盘，GUI 里「会话卡片」重启后就全没了，而且**没有任何
    // 日志**提示，看起来像「服务没记住」。
    //
    // 路径由调用方给（main.cpp 按 config.toml 所在目录拼 data/conversations.json），
    // 这里再兜一次底：父目录不存在就建。
    {
      std::error_code ec;
      const fs::path parent = fs::path(path_).parent_path();
      if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
          ::fish::log::log_line(::fish::log::LogLevel::kWarn, "session",
                                "conversations_dir_failed")
              .field("path", path_)
              .field("detail", ec.message());
        }
      }
    }
    load();
  }

  // 退出时补一次落盘：touch() 不再每条消息写盘，最后一段时间的登记
  // 只存在内存里，进程正常退出时不能丢。
  ~ConversationBook() { flush(); }

  // 启动时登记 bot 本身。已启用的渠道各占一张卡片——
  // 不必等第一条消息进来，人格页一打开就能看到现有的 bot。
  void register_bot(const std::string& key, const std::string& channel,
                    const std::string& title, const std::string& ident) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      // 已有（之前收过消息）：只刷新展示名，不动计数
      it->second.title = title;
    } else {
      Entry e;
      e.key = key;
      e.channel = channel;
      e.kind = "bot";
      e.title = title;
      e.sender_id = ident;
      map_.emplace(e.key, e);
    }
    dirty_ = true;
    save_locked();
  }

  // 有消息进来就刷新内存。线程安全。
  //
  // handled=false 时不动 handled 计数：touch() 在去重之前调用
  // （有意保留——被静默的会话也要登记），所以「收到过消息」不等于
  // 「处理过消息」。想让计数只算真正处理的消息，调用方应在去重/频率
  // 限制之后调 bump_handled()，或在 touch 里传 handled=false。
  // 默认 true 保持原有「来一条算一条」的可见行为，不静默改变 GUI 数字。
  //
  // 这里只改内存 + 置 dirty_，不写盘：原来每条消息都持全局锁做
  // 「dump 全表 + 写 tmp + rename」，所有会话的并发消息在这把锁上串行，
  // 高频会话直接把消息吞吐压在磁盘 IO 上。落盘交给 flush()（定时调用）。
  void touch(fish::core::Channel channel, const fish::core::InboundMessage& msg,
             bool handled = true) {
    Entry e;
    e.key = session_key(msg);
    e.channel = channel_name(channel);
    e.kind = msg.kind == fish::core::ChatKind::kGroup ? "group" : "private";
    e.title = msg.kind == fish::core::ChatKind::kGroup
                  ? (msg.conversation_title.empty() ? msg.conversation_id
                                                     : msg.conversation_title)
                  : (msg.sender_name.empty() ? msg.sender_id : msg.sender_name);
    e.sender_name = msg.sender_name;
    e.sender_id = msg.sender_id;
    e.last_active_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(e.key);
    if (it != map_.end()) {
      // 名字以最新一次为准
      it->second.title = e.title;
      it->second.sender_name = e.sender_name;
      it->second.sender_id = e.sender_id;
      it->second.last_active_ms = e.last_active_ms;
      if (handled) it->second.handled += 1;
    } else {
      e.handled = handled ? 1 : 0;
      map_.emplace(e.key, e);
    }
    dirty_ = true;
  }

  // 真正处理完一条消息后单独记账（去重 / 频率限制之后再调）。
  void bump_handled(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return;
    it->second.handled += 1;
    dirty_ = true;
  }

  // 把内存里的登记表落盘。dirty_ 为假时直接返回，代价只有一次加锁。
  //
  // 由调用方定时调用（main.cpp 的维护定时器），而不是每条消息一次：
  // 会话列表只用于 GUI 展示，晚几秒落盘没有影响。
  void flush() {
    std::lock_guard<std::mutex> lock(mu_);
    save_locked();
  }

  [[nodiscard]] std::vector<Entry> list() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Entry> out;
    out.reserve(map_.size());
    for (const auto& [k, v] : map_) out.push_back(v);
    // 最近活跃的排前面
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) {
                return a.last_active_ms > b.last_active_ms;
              });
    return out;
  }

  // 该渠道 bot 卡片的 key（register_bot 时登记的）。
  // 人格解析用：会话没有覆盖时，退到 bot 级人格。
  // QQ 有两张可能的卡（官方 / 小号），按前缀顺序匹配先登记的那张。
  [[nodiscard]] std::string bot_key_for(fish::core::Channel channel) const {
    const std::vector<std::string> prefixes =
        channel == fish::core::Channel::kWeChat
            ? std::vector<std::string>{"wx:"}
            : std::vector<std::string>{"qq:", "ob:"};
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& p : prefixes) {
      for (const auto& [k, v] : map_) {
        if (v.kind == "bot" && k.rfind(p, 0) == 0) return k;
      }
    }
    return {};
  }

 private:
  void load() {
    std::ifstream f(path_);
    if (!f) {
      // 文件不存在是正常的首次运行，不报错；但要说一声便于排查
      // 「卡片全没了」到底是没有文件还是解析失败。
      ::fish::log::log_line(::fish::log::LogLevel::kDebug, "session",
                            "conversations_absent")
          .field("path", path_);
      return;
    }
    try {
      const auto j = nlohmann::json::parse(f);
      for (const auto& item : j.value("conversations", nlohmann::json::array())) {
        Entry e;
        e.key = item.value("key", "");
        if (e.key.empty()) continue;
        e.channel = item.value("channel", "");
        e.kind = item.value("kind", "");
        e.title = item.value("title", "");
        e.sender_name = item.value("sender_name", "");
        e.sender_id = item.value("sender_id", "");
        e.last_active_ms = item.value("last_active_ms", std::uint64_t{0});
        e.handled = item.value("handled", std::uint64_t{0});
        map_.emplace(e.key, e);
      }
    } catch (const std::exception& e) {
      // 不能静默：文件半截（上次写盘被中断）时所有卡片会消失，
      // 而 GUI 看起来只是「没有会话」，根本无从排查。留 warn + 保留
      // 现场（.bak 是上一次的完整内容）。
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "session",
                            "conversations_parse_failed")
          .field("path", path_)
          .field("detail", e.what())
          .field("hint", std::string("可从 ") + path_ + ".bak 恢复");
    }
  }

  // 全量重写。会话数不会大（见过的 bot/群），flush() 由维护定时器调用，
  // 不再跟消息频率挂钩。
  void save_locked() {
    if (!dirty_) return;
    nlohmann::json j;
    auto arr = nlohmann::json::array();
    for (const auto& [k, v] : map_) {
      arr.push_back({{"key", v.key},
                     {"channel", v.channel},
                     {"kind", v.kind},
                     {"title", v.title},
                     {"sender_name", v.sender_name},
                     {"sender_id", v.sender_id},
                     {"last_active_ms", v.last_active_ms},
                     {"handled", v.handled}});
    }
    j["conversations"] = arr;
    const std::string text = j.dump(2);
    const std::string tmp = path_ + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      // 静默失败是「登记表永远不落盘」最难查的一环：
      // 打不开文件只有一个原因——目录不存在或没权限。
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "session",
                            "conversations_write_failed")
          .field("path", tmp)
          .field("hint", std::string("检查目录是否存在且可写: ") +
                            fs::path(path_).parent_path().string());
      return;
    }
    f << text;
    f.close();
    std::error_code ec;
    fs::rename(tmp, path_, ec);
    if (!ec) {
      dirty_ = false;
      return;
    }
    // rename 失败（跨盘等）退化为直接写。直接写不是原子的，
    // 中途崩了正式文件就是半截——先留一份旧内容的 .bak 再写。
    backup_existing();
    std::ofstream f2(path_, std::ios::binary | std::ios::trunc);
    if (f2) {
      f2 << text;
      f2.close();
      if (f2) dirty_ = false;
    } else {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "session",
                            "conversations_write_failed")
          .field("path", path_)
          .field("hint", std::string("直接写也失败，检查目录权限: ") +
                            fs::path(path_).parent_path().string());
    }
  }

  // 把现有文件复制成 .bak。写失败不阻塞主流程：备份只是补偿措施。
  void backup_existing() const {
    std::ifstream src(path_, std::ios::binary);
    if (!src) return;
    std::ofstream bak(path_ + ".bak", std::ios::binary | std::ios::trunc);
    if (bak) bak << src.rdbuf();
  }

  std::string path_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> map_;
  bool dirty_{false};
};

}  // namespace fish::session
