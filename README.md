# super-feiyu

C++20 实现的大肥鱼（DeepSeek 拟人化鲸鱼娘）机器人内核。

**设计目标**：跑在 2C1G 的 Ubuntu 上，本机零推理，所有能力走远程 API，
只做协议转发与数据落盘。

---

## 为什么不用现成方案

主流方案是 **AstrBot + NapCat**。实测 NapCat 空闲就要 532 MiB，
AstrBot 323 MiB，合计约 855 MiB，1G 机器必 OOM。

本项目的取舍：

| 项 | 现成方案 | 本项目 |
|---|---|---|
| 语言 | Python | C++20 |
| 协议端 | NapCat（套壳 NTQQ，532MB） | 直连官方 API，无协议端 |
| 推理 | 部分本地 | **全部远程** |
| 内存 | 855 MB+ | 目标 < 100 MB |
| 职责边界 | 插件混在一起 | **五层分离，编译期强制** |

---

## 架构：五个职责，单向依赖

```
                    ┌─────────────────────────┐
                    │   orchestrator::Pipeline │  ← 唯一持有业务决策
                    │   「要不要回、回什么」    │
                    └───────────┬─────────────┘
                                │ 只依赖接口
        ┌───────────┬───────────┼───────────┬───────────┐
        ▼           ▼           ▼           ▼           ▼
   IChannel    IChatModel   IMemoryStore  IPersona  IReplyBudget
   （传输）    （推理）      （存储）      （人格）   （预算）
        │           │           │
        ▼           ▼           ▼
   QQ/微信     DeepSeek      SQLite/内存
  官方 API    硅基流动
              Edge-TTS
```

**依赖方向永远指向 `core`。** `core` 不知道 QQ 存在，也不知道 DeepSeek 存在。
换通道、换模型厂商、换存储后端，只换实现，编排层零改动。

### 五层职责

| 层 | 职责 | 不知道什么 |
|---|---|---|
| `core` | 领域类型 + 接口契约 | 任何具体实现 |
| `transport` | 平台协议 ↔ InboundMessage | 业务判断 |
| `capability` | 远程 API 调用 | 消息从哪来 |
| `store` | 落盘、召回 | 模型怎么用记忆 |
| `persona` | 提示词分层拼装 | 网络 |
| `orchestrator` | 串起链路、做决策 | 协议细节 |

---

## 关键设计决策

### 1. 被动对话是红线，且有测试保护

群聊只有被 @ 才回。这条规则写进 `ReplyPolicy`，并有单测：
`group_not_mentioned_is_silent` 断言未 @ 时必须静默。

规则不散落在各通道里，避免新增通道时忘记实现。

### 2. 人格提示词强制分两层

这是**成本生死线**。DeepSeek 缓存命中的输入 0.02 元/百万，
未命中 1 元/百万，**差 50 倍**。

`LayeredPersona` 把提示词拆成：

- `stable_prefix()`：逐字节固定，构造时拼好，之后只读
- `dynamic_suffix()`：时间、召回记忆、本轮红线

任何把时间戳拼进前缀的写法都会废掉缓存。分层是接口强制，不是约定。

### 3. 回复预算独立成层

QQ 官方限制：单聊 60 分钟内 4 条，群聊 5 分钟内 5 条。
超了就整条发送失败。

`ReplyBudget` 按 `message_id` 记次数，带窗口过期回收。
`Pipeline` 在发送前逐条 `consume()`，超预算直接停，而不是等通道报错。

### 4. 统一 Result，异常不跨模块

所有跨模块调用返回 `Result<T>`，携带 `ErrorCode`。
禁止异常穿过模块边界——1G 机器上崩溃重启代价高，
错误必须可归类、可降级、可观测。

### 5. 语音转写失败要降级，不能丢消息

`normalize_input` 里，语音块在没有 STT 时降级为
`[语音消息，未启用转写]` 占位文本，转写失败降级为
`[语音消息转写失败]`。**绝不静默丢消息**。

---

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

要求：C++20 编译器（g++ 13+ / clang++ 16+ / MSVC 19.35+）、CMake 3.20+、Ninja。

编译选项开了 `-Wall -Wextra -Wpedantic -Werror`，零警告。

---

## 当前状态

**已完成**（可编译、测试通过）：

- 领域类型与五层接口契约
- `ReplyPolicy` 被动准入判定 + 单测
- `ReplyBudget` 回复预算 + 单测
- `LayeredPersona` 分层提示词
- `InMemoryStore` 环形缓冲 + 余弦召回
- `Pipeline` 完整处理链路编排

**待实现**：

- `transport/`：QQ 官方机器人 WS 通道、微信 iLink 通道
- `capability/`：DeepSeek / 硅基流动 / Edge-TTS 的 HTTP 客户端
- `store/`：SQLite 持久化实现（现为内存版）
- HTTP 层选型（暂定 Boost.Asio 或直接 libcurl + 自建事件循环）
- 配置加载（TOML/JSON）+ 密钥走环境变量

---

## 目录

```
include/fish/
  core/         types.hpp  interfaces.hpp  reply_budget.hpp
  transport/    （待实现）
  capability/   （待实现）
  store/        memory_store.hpp
  persona/      layered_persona.hpp
  orchestrator/ pipeline.hpp
src/            对应实现
tests/          test_policy.cpp
```

## 许可

MIT
