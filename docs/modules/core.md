# core 模块设计

> 职责：定义领域类型与接口契约。**不实现任何业务，不依赖任何第三方库。**

---

## 1. 定位

`core` 是整个系统的**唯一稳定内核**。所有其他模块依赖它，它不依赖任何人。

**硬约束**：`core` 的头文件**不得 include** 任何第三方库（http、sqlite、json 等）。
只允许标准库。这条约束由 CMake 强制：`fish_core` target 不链接任何外部库。

---

## 2. 领域类型

### 2.1 消息模型

```cpp
enum class Channel : uint8_t { kQQ, kWeChat };
enum class ChatKind : uint8_t { kPrivate, kGroup };
enum class BlockKind : uint8_t { kText, kImage, kVoice };

struct ContentBlock {
  BlockKind kind;
  std::string text;        // kText 正文
  std::string media_url;   // kImage/kVoice 资源地址
  std::string mime;
};

struct InboundMessage {
  Channel channel;
  ChatKind kind;
  std::string conversation_id;   // 群号 / 单聊用户 id
  std::string sender_id;
  std::string sender_name;
  std::string message_id;        // 被动回复凭证
  std::vector<ContentBlock> blocks;
  bool mentioned_me;
  TimePoint received_at;
  std::string context_token;     // 微信 iLink 需要回填
};
```

**设计要点**：

| 决策 | 理由 |
|---|---|
| 多模态用统一 `ContentBlock` | 避免为图片/语音各开一套并行接口 |
| `message_id` 必填 | 被动回复的凭证，缺了发不出去 |
| `mentioned_me` 由通道判定 | 平台差异（@ 语法）不该泄漏到编排层 |
| `received_at` 由通道填 | 用于时效检查，不能用处理时间 |
| `context_token` 允许为空 | QQ 不需要，微信需要 |

### 2.2 通道能力

```cpp
struct ChannelCapability {
  bool supports_group, supports_private;
  bool supports_image_out, supports_voice_out, supports_voice_in;
  int max_replies_per_message;   // QQ: 私聊 4 / 群聊 5
};
```

**为什么需要它**：编排层要在**发送前**知道这条路能不能发语音，
而不是发出去失败了再降级。能力声明让降级发生在决策阶段。

### 2.3 Result 与错误

```cpp
enum class ErrorCode : uint8_t {
  kOk, kTimeout, kRateLimited, kUnauthorized, kBadRequest,
  kUpstreamUnavailable, kBudgetExhausted, kUnsupported, kInternal,
};

struct Error { ErrorCode code; std::string detail; };

template <typename T> struct Result {
  std::optional<T> value;
  Error error;
};
```

---

## 3. 接口契约

### 3.1 IChannel

```cpp
class IChannel {
 public:
  virtual Channel id() const noexcept = 0;
  virtual ChannelCapability capability() const = 0;
  virtual Result<InboundMessage> poll() = 0;      // 阻塞，无消息返回空
  virtual Error send(const OutboundMessage&, int attempt_no) = 0;
};
```

**契约**：
- `poll()` **允许阻塞**，由通道线程独占调用
- **禁止**在通道层做业务判断（「要不要回」不属于这里）
- `attempt_no` 从 1 开始，用于被动回复的 `msg_seq` 递增（QQ 要求同 msg_id 多次回复时递增）

### 3.2 IChatModel

```cpp
class IChatModel {
 public:
  struct Request {
    std::string system_prompt;          // 恒定，命中前缀缓存
    std::vector<ContentBlock> history;  // 追加式
    std::vector<ContentBlock> user_blocks;
    int max_tokens; double temperature;
  };
  struct Response {
    std::string text;
    int prompt_tokens, cached_tokens, output_tokens;
  };
  virtual Result<Response> complete(const Request&) = 0;
};
```

**为什么把 `system_prompt` 和 `history` 分开**：
强制实现方把 system 放最前面。如果合成一个 `messages` 数组，
实现方很容易把动态内容混进前面，废掉缓存。**这是架构级防呆。**

`Response` 带 `cached_tokens`：成本观测的唯一数据源。

### 3.3 其余接口

```cpp
class IEmbeddingModel   { Result<vector<float>> embed(const string&) = 0; int dimension() const; };
class ISpeechToText     { Result<string> transcribe(const string& media_url) = 0; };
class ITextToSpeech     { Result<string> synthesize(const string& text) = 0; };
class IMemoryStore      { Error append(...); Result<...> recall(...); Result<...> recent(...); };
class IPersona          { string stable_prefix() const; string dynamic_suffix(...) const; };
class IReplyBudget      { int remaining(...) const; bool consume(...) = 0; };
```

---

## 4. 已发现的实现陷阱

### 4.1 命名空间限定

头文件中 `Result<T>` 位于 `fish::core`，在 `fish::orchestrator`、
`fish::persona` 等命名空间里**必须写 `core::Result<T>`**，否则编译失败。

> 这是实际踩过的坑：初版 `pipeline.hpp` 直接写 `Result<int>`，
> `-Werror` 下编译中断。修复方式是全限定。

### 4.2 接口文件不能有占位垃圾

曾出现过 `IMemoryStore` 只写了 `virtual ~ITextToSpeech_dummy() = delete;`
的占位实现（应为 `IMemoryStore` 的完整定义）。**接口文件必须一次写全**，
占位会掩盖设计缺口。

### 4.3 `-Wpedantic -Werror` 下的常见问题

- 枚举与 `uint8_t` 互转需要显式 `static_cast`
- 未使用的参数必须 `[[maybe_unused]]` 或删名
- 结构体初始化顺序必须与声明顺序一致

---

## 5. 待定事项

| 事项 | 当前倾向 | 需要决策 |
|---|---|---|
| `ContentBlock` 是否加 `metadata` map | 不加，用具体字段 | 多模态扩展时会需要 |
| `TimePoint` 用 system_clock 还是 steady_clock | system_clock | 要看墙钟时间做时效判断 |
| `Result<T>` 是否支持 `and_then` 链式 | 暂不加 | 减少嵌套 if |
| 错误是否带 HTTP status | 不放 core | 属于 capability 层细节 |

---

## 6. 测试要点

| 用例 | 断言 |
|---|---|
| `Result` 成功/失败构造 | `.ok()` 与 `.value` 正确 |
| 枚举默认值 | 未初始化时行为确定 |
| `stable_prefix()` 幂等 | 两次调用逐字节相等（缓存前置条件） |

**不测**：接口本身无需测试，测的是实现。
