# ADR-004：不用异常，统一 Result

**状态**：已定

---

## 决策

跨模块调用一律返回 `Result<T>` 或 `Error`，**禁止异常穿过模块边界**。

## 理由

### 1. 降级策略要求错误可归类

上游失败分很多种，处理方式完全不同：

| 类型 | 处理 |
|---|---|
| 超时 / 429 / 5xx | 退避重试 |
| 4xx | 不重试，降级 |
| 预算耗尽 | 静默 |

异常只有一个 `what()` 字符串，靠字符串匹配分类非常脆弱。
`ErrorCode` 枚举让分类在编译期确定。

### 2. 1G 机器崩溃代价高

异常没被捕获 → `std::terminate` → 进程退出 → 用户消息丢失。
在这个场景下，"降级回复一句废话"远好于"进程挂掉"。

### 3. C++ 异常有运行时开销

每个 try 块都影响优化与代码体积。
在嵌入式级资源约束下，`Result` 的显式分支更可控。

## 接口

```cpp
enum class ErrorCode : uint8_t {
  kOk, kTimeout, kRateLimited, kUnauthorized, kBadRequest,
  kUpstreamUnavailable, kBudgetExhausted, kUnsupported, kInternal,
};

template <typename T>
struct Result {
  std::optional<T> value;
  Error error;
  bool ok() const noexcept { return error.ok(); }
};
```

## 代价

| 代价 | 缓解 |
|---|---|
| 代码嵌套变深 | 提供 `and_then` 链式（待定） |
| 忘记检查返回值 | `[[nodiscard]]` 标记 |
| 无栈回溯 | 日志里带 context |

## 纪律

- 所有 `Result` 必须 `[[nodiscard]]`
- 每个错误分支必须有日志或 metric
- **禁止** `if (!r.ok()) {}` 空处理
