# ADR-003：前缀缓存优先，强制人格分层

**状态**：已定
**日期**：2026-11-12

---

## 背景

DeepSeek 定价（元 / 百万 token）：

| | 缓存命中 | 缓存未命中 | 倍数 |
|---|---|---|---|
| 输入 | 0.02 | 1 | **50×** |
| 输出 | 4 | 4 | 1× |

人格 prompt 通常 3000–5000 token。每次请求都要带上。

## 问题

若人格 prompt 每轮变化（哪怕只变一个时间戳），缓存永远不命中：

| 场景 | 每天成本（100 轮） | 每月 |
|---|---|---|
| 缓存命中 | 0.01 元 | **0.3 元** |
| 缓存未命中 | 0.5 元 | **15 元** |

差 50 倍。更糟的是**这个失效是静默的**：
不会报错，不会告警，只会让账单慢慢变贵，作者往往几个月后才发现。

## 决策

**在接口层强制分层，让"把动态内容拼进前缀"变成做不到的事。**

```cpp
class IPersona {
  [[nodiscard]] virtual std::string stable_prefix() const = 0;   // 只读
  [[nodiscard]] virtual std::string dynamic_suffix(...) const = 0; // 每轮
};

class IChatModel {
  struct Request {
    std::string system_prompt;          // 与 history 分开，强制置前
    std::vector<ContentBlock> history;
    std::vector<ContentBlock> user_blocks;
  };
};
```

## 关键设计点

### 1. `stable_prefix()` 返回 `const std::string`

不是返回值语义，而是**接口语义**：想往里加东西做不到。

### 2. `system_prompt` 与 `messages` 分离

如果只给一个 `messages` 数组，实现方很容易把动态内容混进第一条。
分离后，实现方被迫把 system 单独放最前面。

### 3. 红线放尾部而非前缀

行为红线（"只回一条、不主动开话题"）若放前缀，会占缓存 token；
放尾部只占少量未命中 token，且利用近因效应执行力更强。

## 后果

### 必需

- 单测断言 `stable_prefix()` 两次调用逐字节相等
- 运行时监控 `cache_hit_ratio = cached_tokens / prompt_tokens`
- 低于 0.7 告警

### 限制

- 人格内容**热更新会清空缓存**，所以人格只支持重启生效
- 时间粒度只能到小时（秒级虽在尾部，但会增加尾部长度）
- 召回记忆必须由编排层注入尾部，persona 不自己去查

## 被否决的替代方案

| 方案 | 否决理由 |
|---|---|
| 依赖开发者自觉 | 已经因此踩过坑，自觉不可靠 |
| 事后监控告警 | 只能发现，不能阻止；等发现时钱已花掉 |
| 缩短人格 prompt | 治标，且牺牲人设质量 |

## 验证

`cache_hit_ratio` 是**第一健康指标**：

| 区间 | 判断 |
|---|---|
| > 0.85 | 正常 |
| 0.70–0.85 | 有动态内容泄漏嫌疑 |
| < 0.70 | 明确违规，必须排查 |
