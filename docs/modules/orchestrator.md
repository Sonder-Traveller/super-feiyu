# orchestrator 模块设计

> 职责：串起全部链路，**唯一持有业务决策**。
> 它是系统里唯一允许回答"要不要回、回什么、怎么回"的地方。

---

## 1. 定位

编排层是**策略层**，不是能力层。它自己不调 HTTP、不碰数据库，
只决定"调谁、按什么顺序、失败了怎么办"。

```
transport ──InboundMessage──▶ orchestrator ──▶ capability
                                   │
                                   ├──▶ store
                                   ├──▶ persona
                                   └──▶ transport.send
```

---

## 2. 处理链路（十二步）

```
1.  去重           message_id / 内容哈希
2.  时效检查        被动窗口剩余 < 20% → 丢弃
3.  准入判定        ReplyPolicy（群聊未 @ → 静默）
4.  预算检查        remaining(msg_id) <= 0 → 丢弃
5.  多模态归一化     语音 → STT → 文本；图片透传
6.  记忆召回        embedding → store.recall + store.recent
7.  提示词组装      persona.stable_prefix + 历史 + dynamic_suffix
8.  模型调用        capability.complete（带熔断）
9.  落盘            用户消息 + 助手回复双写
10. 语音合成（可选） TTS，失败只发文本
11. 预算扣减        逐条 consume
12. 发送            transport.send，带 msg_seq
```

**每一步都可以提前终止**，但终止原因必须记录。

---

## 3. 准入判定（ReplyPolicy）

### 3.1 规则

```cpp
Decision ReplyPolicy::evaluate(const InboundMessage& msg,
                               const ChannelCapability& cap) const {
  if (msg.kind == ChatKind::kPrivate) {
    if (!cap.supports_private) return {false, "private_unsupported"};
    return {true, "private"};
  }
  if (!cap.supports_group) return {false, "group_unsupported"};
  if (!msg.mentioned_me) return {false, "group_not_mentioned"};  // ← 核心红线
  return {true, "group_mentioned"};
}
```

### 3.2 为什么独立成类

| 理由 | 说明 |
|---|---|
| 可单测 | 不依赖任何 IO |
| 可替换 | 将来要"群聊也回"只需换策略 |
| 原因可观测 | `reason` 字段直接进 metric |

**红线必须有测试**：`group_not_mentioned_is_silent`。
这条规则一旦被改坏，机器人会在群里对每条消息插嘴，迅速被踢。

### 3.3 不做的事

- ❌ 不做内容审核（交给上游）
- ❌ 不做频率限制（交给预算层）
- ❌ 不做"这条值不值得回"的语义判断（先简单）

---

## 4. 回复预算（ReplyBudget）

### 4.1 平台约束

QQ 官方（被动消息）：

| 场景 | 有效期 | 可回复次数 |
|---|---|---|
| 单聊 | 60 分钟 | 4 次 |
| 群聊 | 5 分钟 | 5 次 |

**超了整条发送失败**，不是排队等。

### 4.2 实现要点

```cpp
class ReplyBudget {
  struct Entry { int used; TimePoint expires_at; };
  std::unordered_map<std::string, Entry> entries_;  // key = message_id
  mutable std::mutex mu_;

  int remaining(const std::string& id) const;
  bool consume(const std::string& id);
  size_t sweep();   // 清理过期条目
};
```

**必须定期 `sweep()`**：否则 `entries_` 随消息数无限增长，
长期运行 OOM。定时线程每小时调一次。

### 4.3 超预算时的行为

**不是"少发几条"，而是"发最重要的那条"**。

优先级：
1. 文本回复（必发）
2. 语音（可选，占配额）
3. 表情包（可选）

所以 `Pipeline` 里要**先扣文本的预算**，语音再看剩余。
不能先合成语音把预算吃掉，导致正文发不出去。

---

## 5. 去重

### 5.1 为什么需要

平台会重发事件：
- WS 断线重连后可能重推送
- 长轮询超时重复拉取
- 网络抖动导致 ACK 丢失

不去重会导致**重复回复**，非常明显。

### 5.2 实现

```cpp
// LRU 缓存，容量 1024，TTL 5 分钟
class DedupCache {
  bool seen(const std::string& message_id);  // 见过返回 true 并刷新时间
};
```

**只用 message_id**，不用内容哈希：
同一句话发两次是两条不同消息，都应该回。

### 5.3 内存

1024 条 × 约 80 字节 ≈ 80 KB。可接受。

---

## 6. 会话串行化

### 6.1 问题

两个工作线程同时处理同群两条消息：
- 回复顺序可能颠倒
- 记忆写入竞争
- 上下文可能不一致

### 6.2 方案：条带锁

```cpp
std::array<std::mutex, 64> shards_;

auto& lock_for(const std::string& conv_id) {
  return shards_[std::hash<std::string>{}(conv_id) % 64];
}
```

**为什么不用每会话一个队列**：会话数不可控（可能上千个群），
每会话一个 mutex + queue 会占用大量内存。64 个条带是固定开销。

**碰撞代价**：两个不相关会话哈希到同一槽时会短暂串行。
64 个槽在百级会话下碰撞率可接受，且只是延迟增加，不影响正确性。

---

## 7. 降级策略（失败语义）

**核心原则：任何一步失败都不能导致「不回复」，除非策略明确要求静默。**

| 失败点 | 行为 | 用户感知 |
|---|---|---|
| 去重命中 | 静默 | 无（正确） |
| 消息过期 | 静默 | 无（已无意义） |
| 策略不回 | 静默 | 无（符合预期） |
| STT 失败 | 占位文本继续 | 知道语音没听清 |
| 记忆召回失败 | 无记忆上下文 | 无感 |
| **ChatModel 失败** | **固定降级文案** | 她"卡了一下" |
| TTS 失败 | 只发文本 | 无语音 |
| 发送失败 | 重试后放弃 | 可能收不到 |

### 7.1 降级文案

不要用技术性错误。用符合人设的一句话：

```
"我卡了一下…你刚才说什么？"
```

**不要**：`"Error: upstream timeout"`。

---

## 8. 时空边界

### 8.1 时效检查

被动回复窗口有限，处理太慢会来不及。

```cpp
const auto elapsed = now - msg.received_at;
if (elapsed > window * 0.8) {
  return ok(0);  // 主动放弃，避免发出去也失败
}
```

窗口值从 `ChannelCapability` 或配置读取：
- QQ 群：5 min → 超过 4 min 放弃
- QQ 私聊：60 min → 超过 48 min 放弃

### 8.2 超时预算

整个链路的总耗时目标 **< 15 s**：

| 阶段 | 预算 |
|---|---|
| STT | 3 s |
| Embedding + 召回 | 1 s |
| ChatModel | 8 s |
| TTS | 3 s |

超时的阶段直接降级跳过。

---

## 9. 与各模块的契约

| 依赖 | 契约 | 失败处理 |
|---|---|---|
| `IChannel::poll` | 允许阻塞 | 线程退出时返回错误 |
| `IChatModel::complete` | 返回 `Result<Response>` | 重试 → 降级文案 |
| `IMemoryStore::append` | 返回 `Error` | 记日志，不阻断回复 |
| `IMemoryStore::recall` | 返回 `Result` | 无记忆继续 |
| `IPersona::stable_prefix` | 纯函数，不失败 | — |
| `IReplyBudget::consume` | 返回 bool | false 则停发 |

**注意**：`store.append` 失败**不阻断**回复。
记忆丢了是遗憾，但不回消息是事故。

---

## 10. 现有实现状态

已实现（`src/pipeline.cpp`）：

- ✅ `ReplyPolicy::evaluate` 完整规则
- ✅ `Pipeline::normalize_input` 多模态降级
- ✅ `Pipeline::build_history` recent + recall 合并
- ✅ `Pipeline::handle` 主链路（12 步中的 5/6/7/8/9/11/12）
- ✅ 预算裁剪（超限截断 blocks）

**待补**：

- ⬜ 去重（`DedupCache`）
- ⬜ 时效检查
- ⬜ 会话条带锁
- ⬜ 熔断
- ⬜ 阶段超时控制
- ⬜ 降级文案
- ⬜ 指标埋点

---

## 11. 待定事项

| 事项 | 选项 | 倾向 |
|---|---|---|
| 是否支持多轮工具调用 | 支持 / 不支持 | 不支持（被动场景用不上） |
| 长回复分段 | 支持 / 截断 | 截断（分段吃配额） |
| 表情包触发 | 概率 / 情绪判定 | 概率（简单可控） |
| 群聊上下文窗口 | 8 条 / 20 条 | 8 条（省 token） |
| 是否需要"接话"语义判断 | 需要 / 不需要 | 不需要（被动就是被动） |

---

## 12. 测试要点

| 用例 | 断言 |
|---|---|
| 群聊未 @ 不回 | `handle` 返回 0，通道未调用 send |
| 群聊被 @ 会回 | 返回 ≥ 1 |
| 私聊必回 | 返回 ≥ 1 |
| 预算耗尽不发 | 第 5 条后 `send` 不被调用 |
| STT 失败降级 | 模型收到占位文本，消息仍发出 |
| 模型失败降级 | 返回降级文案而非错误 |
| 落盘失败不阻断 | `append` 返回错误，仍发送成功 |
| 同会话串行 | 并发两条消息，断言不交错 |
| 去掉重 | 同一 message_id 两次，只回一次 |

**去掉重和串行化是最容易回归的两项**，必须有测试守住。
