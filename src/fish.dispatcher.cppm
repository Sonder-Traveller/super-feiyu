// src/fish.dispatcher.cppm
// 消息调度器：把「收消息」和「处理消息」解耦。
//
// 架构：
//   通道线程（每通道一个）只做 poll，拿到消息立刻投递给线程池，
//   然后继续 poll。真正的处理（模型调用、记忆读写、发消息）在
//   worker 线程里跑。
//
//   这样慢的模型调用不会阻塞收消息，多个会话可以真正并行。
//
// 会话隔离：
//   每个任务携带 SessionPtr（会话上下文），worker 执行时先拿会话锁。
//   同会话串行（保证上下文一致、回复顺序正确），
//   不同会话并行（互不阻塞）。
//   记忆落盘也按会话 id 分目录，见 fish.memory 的 NotesStore::session_dir。
//
// 线程池选型：
//   用 Mzy123l/Thread-Pool 的 StrictVariantThreadPool + LockFreeRingQueue。
//   该组合在 README 基准里开销最低（轻量任务 16ms vs function 版 403ms），
//   自实现 StaticFunction/StaticPromise，零虚表、零 RTTI。
//   限制：submit 的返回类型必须精确匹配 variant 里的
//   StaticPackagedTask<T()>，所以这里只用 void() 和 int() 两种任务。
module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

#include "lock_free_ring_queue.hpp"
#include "lock_free_strict_variant_thread_pool.hpp"

export module fish.dispatcher;

import fish.core;
import fish.log;
import fish.session;

export namespace fish::dispatcher {

// 一个待处理的入站任务。值语义，跟着任务走，worker 拿到什么就是什么。
struct Task {
  fish::core::InboundMessage msg;
  fish::session::SessionPtr session;
  std::string channel_name;
};

// 处理回调：返回 1 表示处理成功，0 表示跳过，-1 表示失败。
// 用 int 而非 bool，是为了在 future 里能区分"跳过"和"成功"。
using Handler = std::function<int(Task&)>;

class Dispatcher {
 public:
  struct Config {
    // 工作线程数。0 表示按硬件并发数（但至少 2）。
    std::size_t threads{0};
    // 队列容量，必须是 2 的幂。满了会阻塞提交方（背压），
    // 这是有意的：宁可让通道线程等一下，也不要丢消息。
    static constexpr std::size_t kQueueCapacity = 1024;
  };

  explicit Dispatcher(Config cfg);
  ~Dispatcher();

  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;

  // 注册处理器。必须在 submit 之前调用。
  void set_handler(Handler h);

  // 提交任务。阻塞直到入队成功（队列满时等待）。
  // 返回 false 表示调度器已关闭。
  bool submit(Task task);

  // 等待所有已提交任务完成
  void drain();

  // 优雅关闭：不再接受新任务，等待进行中的完成
  void shutdown();

  // 统计
  [[nodiscard]] std::uint64_t submitted() const noexcept {
    return submitted_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t completed() const noexcept {
    return completed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t failed() const noexcept {
    return failed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::size_t threads() const noexcept { return threads_; }

 private:
  // 任务 variant：只放两种返回类型，够用且最省
  using TaskVariant = std::variant<thread_pool::StaticPackagedTask<int>,
                                   thread_pool::StaticPackagedTask<void>>;
  using Queue = lock_free_container::LockFreeRingQueue<TaskVariant,
                                                       Config::kQueueCapacity>;
  using Pool = thread_pool::StrictVariantThreadPool<TaskVariant, Queue>;

  Config cfg_;
  std::size_t threads_{0};
  std::unique_ptr<Pool> pool_;
  Handler handler_;
  std::atomic<bool> closed_{false};
  std::atomic<std::uint64_t> submitted_{0};
  std::atomic<std::uint64_t> completed_{0};
  std::atomic<std::uint64_t> failed_{0};
};

// ── 实现 ────────────────────────────────────────────────

Dispatcher::Dispatcher(Config cfg) : cfg_(cfg) {
  threads_ = cfg.threads;
  if (threads_ == 0) {
    const auto hw = std::thread::hardware_concurrency();
    threads_ = hw == 0 ? 2 : static_cast<std::size_t>(hw);
    // 至少 2 个，保证一个慢会话不拖死所有其他会话
    if (threads_ < 2) threads_ = 2;
  }

  // 用环形队列 + 移动构造
  Queue q;
  pool_ = std::make_unique<Pool>(threads_, std::move(q));

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "dispatcher", "started")
      .field("threads", static_cast<long long>(threads_))
      .field("queue_capacity", static_cast<long long>(Config::kQueueCapacity));
}

Dispatcher::~Dispatcher() {
  shutdown();
}

void Dispatcher::set_handler(Handler h) { handler_ = std::move(h); }

bool Dispatcher::submit(Task task) {
  if (closed_.load(std::memory_order_acquire)) return false;

  const std::string key = task.session ? task.session->key() : "?";

  // 提交到池：任务体拿会话锁 → 调 handler
  // 返回 int 以匹配 StaticPackagedTask<int()>
  pool_->submit(
      [this, t = std::move(task)]() mutable -> int {
        if (!handler_) return 0;
        // 同会话串行：锁在任务执行期间持有
        std::unique_lock<std::mutex> lock;
        if (t.session) lock = std::unique_lock<std::mutex>(t.session->lock());

        const int rc = handler_(t);
        if (rc > 0) {
          completed_.fetch_add(1, std::memory_order_relaxed);
        } else if (rc < 0) {
          failed_.fetch_add(1, std::memory_order_relaxed);
        }
        if (t.session) {
          auto& info = t.session->info();
          info.last_active_ms = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count());
          ++info.handled;
        }
        return rc;
      });

  submitted_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void Dispatcher::drain() { pool_->wait_all(); }

void Dispatcher::shutdown() {
  if (closed_.exchange(true, std::memory_order_acq_rel)) return;
  if (pool_) {
    pool_->shutdown();
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "dispatcher", "stopped")
        .field("submitted", static_cast<long long>(submitted_.load()))
        .field("completed", static_cast<long long>(completed_.load()))
        .field("failed", static_cast<long long>(failed_.load()));
  }
}

}  // namespace fish::dispatcher
