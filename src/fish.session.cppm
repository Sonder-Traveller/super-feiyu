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
//   - 每个会话一个 id（群聊 g:<openid>，私聊 <openid>）
//   - 每个会话一把锁：同会话串行，不同会话并行
//   - 记忆落盘按 id 分目录，互不可见
module;

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

export module fish.session;

import fish.core;

export namespace fish::session {

// 会话键：从入站消息推导，全项目统一用这一个函数，
// 避免各处自己拼字符串导致隔离失效。
[[nodiscard]] inline std::string session_key(const fish::core::InboundMessage& msg) {
  // 群聊必须带 g: 前缀，否则群 openid 和用户 openid 可能撞车
  if (msg.kind == fish::core::ChatKind::kGroup) {
    return "g:" + msg.conversation_id;
  }
  return msg.conversation_id;
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

}  // namespace fish::session
