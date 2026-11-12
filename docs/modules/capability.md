# capability 模块设计（多 Provider 统一接口）

> 职责：把"调用某类远程能力"抽象成统一接口，
> **配置哪一家的 key 都能用**，上层代码零改动。

---

## 1. 核心设计：能力 × 厂商 二维解耦

问题：文本、音频、搜索各有 N 家厂商，接口形态完全不同。

| 能力 | 厂商 A 接口 | 厂商 B 接口 |
|---|---|---|
| 文本 | OpenAI `/chat/completions` | Anthropic `/v1/messages` |
| 语音识别 | OpenAI `/audio/transcriptions` | 自定义 multipart |
| 语音合成 | Edge 私有 WS | OpenAI `/audio/speech` |
| 搜索 | Tavily REST | 博查 REST |

若为每家写一套，上层会有 `if (provider == "x")` 的分支地狱。

### 解决：两层抽象

```
        上层（orchestrator）
              │
              ▼
   ┌──────────────────────┐
   │  统一能力接口         │  IChatModel / ISpeechToText / ...
   │  与厂商无关           │
   └──────────┬───────────┘
              │ 工厂按配置构造
   ┌──────────▼───────────┐
   │  Provider Registry   │  "deepseek" → DeepSeekChat
   │                      │  "openai"   → OpenAIChat
   └──────────┬───────────┘
              │
   ┌──────────▼───────────┐
   │  协议适配器（可复用）  │  OpenAICompat / AnthropicCompat
   └──────────────────────┘
```

**关键洞察**：绝大多数厂商兼容 OpenAI 协议。
所以适配器只有两三个，不是 N 个。

---

## 2. 协议适配器层

### 2.1 为什么按协议而非厂商分

| 协议 | 厂商 |
|---|---|
| OpenAI 兼容 | 硅基流动、DeepSeek（有兼容端点）、火山方舟、阿里、Moonshot、Ollama… |
| Anthropic 兼容 | Anthropic、DeepSeek（anthropic 端点） |
| 私有 | Edge-TTS |

**所以只需要 3 个适配器 + N 份配置。**

### 2.2 适配器接口

```cpp
// 内部接口，不暴露给上层
class IHttpDialect {
 public:
  virtual ~IHttpDialect() = default;
  // 把统一请求翻译成该协议的报文
  virtual std::string build_chat_body(const ChatParams&) = 0;
  // 把该协议响应翻译回统一结构
  virtual Result<ChatResult> parse_chat_response(const std::string&) = 0;
  virtual std::map<std::string,std::string> headers(const std::string& key) = 0;
};
```

实现：
- `OpenAICompatDialect`
- `AnthropicCompatDialect`

---

## 3. 各类能力的统一接口

### 3.1 文本生成 IChatModel

```cpp
struct ChatParams {
  std::string system_prompt;              // 稳定前缀
  std::vector<ContentBlock> history;
  std::vector<ContentBlock> user_blocks;
  int max_tokens = 512;
  double temperature = 1.0;
  bool json_mode = false;                 // 结构化输出
};

struct ChatResult {
  std::string text;
  Usage usage;   // prompt / cached / output
};
```

**统一处理要点**：

| 差异点 | 统一方案 |
|---|---|
| system 位置 | OpenAI 放 messages[0]，Anthropic 放顶层 `system` 字段 |
| 图片格式 | 都转成 `image_url`（Anthropic 转 base64 source） |
| 缓存字段名 | OpenAI 类各家不同，缺失时记 0 |
| JSON 模式 | OpenAI `response_format`，Anthropic 无，靠 prompt 约束 |

**`cached_tokens` 的采集**：各家字段名不一致，在适配器里归一化：

```
DeepSeek:    usage.prompt_cache_hit_tokens
OpenAI:      usage.prompt_tokens_details.cached_tokens
硅基流动:    同 OpenAI
缺失:        0 + 记一次 warn
```

### 3.2 向量 IEmbeddingModel

```cpp
struct EmbeddingParams {
  std::vector<std::string> inputs;   // 支持批量
  int dimension;
};
```

**几乎都是 OpenAI 兼容**，适配成本最低。

### 3.3 语音识别 ISpeechToText

```cpp
struct SttParams {
  std::string media_url;   // 或本地路径
  std::string language;    // "auto" / "zh"
};
```

**免费预设**（按推荐序）：

| Provider | 模型 | 免费额度 | Base URL |
|---|---|---|---|
| `siliconflow` | `FunAudioLLM/SenseVoiceSmall` | 免费（需实名） | `https://api.siliconflow.cn/v1` |
| `groq` | `whisper-large-v3-turbo` | 有免费额度 | `https://api.groq.com/openai/v1` |
| `openai` | `whisper-1` | 无（付费） | `https://api.openai.com/v1` |
| `local` | 任意 OpenAI 兼容 | 自建 | 可配 |

**推荐 siliconflow**：与 embedding 共用一把 key，一个账号解决两项。

### 3.4 语音合成 ITextToSpeech

```cpp
struct TtsParams {
  std::string text;
  std::string voice;
  std::string format = "mp3";
};
```

**免费预设**：

| Provider | 说明 | key |
|---|---|---|
| `edge` | 微软 Edge 在线语音 | **不需要** |
| `siliconflow` | 付费（CosyVoice 等） | 需要 |
| `openai` | `tts-1`，付费 | 需要 |
| `doubao` | 火山豆包语音，约 1 元/分钟 | 需要 |

**默认 edge**：零成本、零配置。风险是非官方接口，需可降级。

### 3.5 搜索 IWebSearch

```cpp
struct SearchParams {
  std::string query;
  int max_results = 5;
  std::vector<std::string> domains;   // 限定站点
};

struct SearchHit {
  std::string title, url, snippet;
  double score;
};
```

**预设**：

| Provider | 价格 | 说明 |
|---|---|---|
| `deepseek` | 5 元/1000 次 | 与对话同 key |
| `tavily` | 免费 1000 次/月 | 海外内容好 |
| `bocha` | 0.036 元/次 | 国内网页全 |
| `searxng` | 免费 | 需自建实例 |
| `none` | — | 关闭搜索 |

**默认 none**：被动对话场景搜索用得少，先关。需要时再开。

---

## 4. 配置：如何做到"配哪家都能用"

### 4.1 配置结构

```toml
# 每类能力独立配置，互不影响
[capability.chat]
provider = "deepseek"          # ← 换这里就能换厂商
base_url = "https://api.deepseek.com"
api_key  = "${DEEPSEEK_API_KEY}"
model    = "deepseek-flash"
max_tokens = 512
temperature = 1.0

[capability.embedding]
provider = "siliconflow"
base_url = "https://api.siliconflow.cn/v1"
api_key  = "${SILICONFLOW_API_KEY}"
model    = "BAAI/bge-m3"
dimension = 1024

[capability.stt]
provider = "siliconflow"
base_url = "https://api.siliconflow.cn/v1"
api_key  = "${SILICONFLOW_API_KEY}"     # 同一把 key
model    = "FunAudioLLM/SenseVoiceSmall"
language = "auto"

[capability.tts]
provider = "edge"                        # 无需 key
voice    = "zh-CN-XiaoyiNeural"
format   = "mp3"
group_voice = false                      # 群里不发语音

[capability.search]
provider = "none"                        # 默认关闭
api_key  = "${TAVILY_API_KEY}"
max_results = 5
```

### 4.2 切换厂商只改两行

```toml
provider = "groq"
base_url = "https://api.groq.com/openai/v1"
```

上层代码完全不动，因为工厂按 `provider` 选适配器。

### 4.3 Key 可以复用的场景

| 场景 | 配置 |
|---|---|
| 一个硅基流动 key 同时做 embedding + STT | 两处都填 `${SILICONFLOW_API_KEY}` |
| 一个 DeepSeek key 同时做对话 + 搜索 | 两处都填 `${DEEPSEEK_API_KEY}` |
| 全部用 OpenAI | 四处都填 `${OPENAI_API_KEY}`，provider 全改 `openai` |

**这是"一套相同接口"的实际价值**：换厂商成本 = 改配置，不是改代码。

---

## 5. 工厂与注册表

```cpp
class CapabilityFactory {
 public:
  // 按配置构造；provider 未知时返回 kUnsupported
  static Result<std::unique_ptr<IChatModel>>
      make_chat(const ChatConfig&);

  static Result<std::unique_ptr<IEmbeddingModel>>
      make_embedding(const EmbeddingConfig&);
  // ... 其余同理
};
```

### 5.1 Provider 注册

```cpp
struct ChatProviderSpec {
  std::string name;                    // "deepseek"
  ChatDialect dialect;                 // OpenAICompat / AnthropicCompat
  std::string default_base_url;
  std::string default_model;
};

const std::map<std::string, ChatProviderSpec> kChatProviders = {
  {"deepseek",    {OpenAICompat,   "https://api.deepseek.com",          "deepseek-flash"}},
  {"siliconflow", {OpenAICompat,   "https://api.siliconflow.cn/v1",     "deepseek-ai/DeepSeek-V3"}},
  {"openai",      {OpenAICompat,   "https://api.openai.com/v1",         "gpt-4o-mini"}},
  {"anthropic",   {AnthropicCompat,"https://api.anthropic.com",         "claude-sonnet-4-5"}},
  {"ollama",      {OpenAICompat,   "http://127.0.0.1:11434/v1",         "qwen2.5"}},
};
```

**默认值自动补全**：配置里只写 `provider`，`base_url` 和 `model` 用默认，
减少必填项。显式写了就用显式的。

### 5.2 未知 provider

启动时报错退出，并列出支持的 provider：

```
Unknown chat provider "claude2". Supported: anthropic, deepseek, ollama, openai, siliconflow
```

**不允许静默降级到默认值** —— 那会让人以为配好了，实际没用。

---

## 6. 通用 HTTP 基础设施

所有能力共用，避免重复实现重试/超时/熔断。

```cpp
struct HttpRequest {
  std::string method, url, body;
  std::map<std::string,std::string> headers;
  int timeout_ms;
  int max_retries;
};
```

### 6.1 重试

| 状态 | 重试 | 退避 |
|---|---|---|
| 超时 / 连接失败 | 2 次 | 500ms, 1500ms |
| 429 | 2 次 | `Retry-After` 或 5s |
| 5xx | 1 次 | 2s |
| 4xx | ❌ | — |

### 6.2 熔断（按 provider 独立）

**为什么按 provider 而不全局**：硅基流动挂了不该影响 DeepSeek 对话。

```cpp
class CircuitBreaker {
  // 每 provider 一个实例
  int failures_ = 0;
  TimePoint open_until_{};
  static constexpr int kThreshold = 5;
  static constexpr auto kCooldown = std::chrono::seconds(30);
};
```

---

## 7. 降级矩阵

**任何能力失败都不能让对话整体失败。**

| 能力 | 失败行为 | 用户感知 |
|---|---|---|
| chat | 重试 → 降级文案 | 她"卡了一下" |
| embedding | 无向量，退化为关键词召回 | 记忆没那么准 |
| stt | 占位文本 `[语音转写失败]` | 知道没听清 |
| tts | 只发文本 | 没有语音 |
| search | 跳过搜索直接回答 | 回答可能不够新 |

### 7.1 embedding 失败的连锁影响

embedding 挂了 → 无法召回 L3 → **但仍可用 L2 短时记忆**。

**这是三层记忆的价值**：L3 依赖外部 API，L2 不依赖。
即使 embedding 长期失效，短期对话仍然正常。

---

## 8. 免费方案汇总（开箱可用）

| 能力 | Provider | 是否需要实名 | 成本 |
|---|---|---|---|
| 对话 | `deepseek` | 否 | ~30–45 元/月 |
| 向量 | `siliconflow` | **是** | 0 |
| 语音识别 | `siliconflow` | **是** | 0 |
| 语音合成 | `edge` | 否 | 0 |
| 搜索 | `none` 或 `tavily` | 否 | 0（1000 次/月内） |

**最小可用组合**：
- 一个 DeepSeek key（付费）
- 一个硅基流动 key（免费，需实名）
- Edge-TTS 无需 key

**两把 key，全部能力可用。**

---

## 9. 依赖与体积

| 库 | 用途 |
|---|---|
| libcurl | HTTP |
| OpenSSL | TLS |
| nlohmann/json | JSON |
| toml++ | 配置 |

静态链接合计约 **8–10 MB**。

**不含**：音频转码（ffmpeg 约 70 MB）。先要求平台原生格式。

---

## 10. 待定事项

| 事项 | 选项 | 倾向 |
|---|---|---|
| HTTP 库 | libcurl / Boost.Beast | **libcurl** |
| 是否支持多 provider 同时 | 每类一个 / 多个 | **每类一个**（简单） |
| 失败是否自动切换备用 provider | 支持 / 不支持 | 暂不支持（靠配置手动切） |
| embedding 本地缓存 | 缓存 / 每次重算 | **缓存**（省钱省时） |
| 音频转码 | 内置 ffmpeg / 外部 / 不做 | 先不做 |
| 流式输出 | 支持 / 不支持 | 不支持（消息要完整才发） |

---

## 11. 测试要点

| 用例 | 断言 |
|---|---|
| OpenAI 适配器请求体 | 生成的 JSON 结构正确 |
| Anthropic 适配器 system 位置 | 在顶层而非 messages |
| 缓存字段归一化 | 各家响应都正确提取 cached_tokens |
| 未知 provider 报错 | 退出码非 0，提示含支持列表 |
| 默认值补全 | 只填 provider 时 base_url/model 正确 |
| 按 provider 独立熔断 | A 熔断不影响 B |
| embedding 失败降级 | 退回关键词召回，对话仍进行 |
| stt 失败降级 | 占位文本，消息仍发出 |
| tts 失败降级 | 只发文本 |
| 429 退避序列 | Mock 连续 429，验证退避 |

**均 Mock HTTP，不打真实 API。**
