# capability 模块设计（多 Provider 统一接口）

> 职责：把"调用某类远程能力"抽象成统一接口，
> **配置哪一家的 key 都能用**，上层代码零改动。

> **文档解析不在本模块**：文档有自己的失败模式（页数超限、加密、解析超时）
> 和成本模型（按页计费），已独立成 `fish.doc` 层。
> 见 [doc.md](doc.md)。

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
  int max_tokens = 0;                      // 0 = 不发送，用上游默认值
  double temperature = -1.0;               // 负数 = 不发送，用上游默认值
  bool json_mode = false;                 // 结构化输出
};

struct ChatResult {
  std::string text;
  Usage usage;   // prompt / cached / output
};
```

> **不主动设上限**：`max_tokens` / `temperature` 默认不发送，
> 让上游用自己的默认值。写死等于替上游设一个它没要求的限制——
> 模型该输出多长、多随机，交给上游和提示词决定。
> 见 [docs/PROMPTS.md](../PROMPTS.md)。

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

**可用 provider**（`stt_provider_list()`）：

| Provider | 模型 | 免费额度 | Base URL |
|---|---|---|---|
| `siliconflow` | `FunAudioLLM/SenseVoiceSmall` | 免费（需实名） | `https://api.siliconflow.cn/v1` |
| `deepseek` | DeepSeek 语音接口 | 按量 | `https://api.deepseek.com` |
| `dashscope` | 阿里 Paraformer | 按量 | `https://dashscope.aliyuncs.com/compatible-mode/v1` |
| `volcengine` | 火山引擎录音文件识别 | 按量 | 火山控制台 |

**推荐 siliconflow**：与 embedding 共用一把 key，一个账号解决两项。

各家差异只有三处：**路径 / 模型名 / 是否强制 WAV**，按 provider 自动套用：

- `need_wav16k = true` —— 强制转 16k 单声道 WAV（百度需要）
- `max_upload_bytes` —— 上传大小上限，0 = 不限（DeepSeek 是 8388608）

### 3.4 语音合成 ITextToSpeech

```cpp
struct TtsParams {
  std::string text;
  std::string voice;
  std::string format = "mp3";
};
```

**可用 provider**（`tts_provider_list()`）：

| Provider | 说明 | key |
|---|---|---|
| `edge` | 本机 edge-tts（微软音色） | **不需要** |
| `siliconflow` | CosyVoice2（voice 要带前缀） | 需要 |
| `dashscope` | CosyVoice（非流式返回音频 URL，两跳） | 需要 |
| `volcengine` | 火山引擎，**用 v1 接口**（v3 是流式的，没有「一次返回完整文件」的形态） | app_id + access_token 两套 |
| `deepseek` | DeepSeek 语音接口 | 需要 |

**默认 edge**：零成本、零配置。风险是非官方接口，需可降级。

火山专用字段：`app_id`（appid）、`cluster`（一般 `volcano_tts`）；
`extra_json` 可覆盖 `audio` 段的 `speed_ratio` / `volume_ratio` 等。

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

**可用 provider**（`search_provider_list()`）：
`bocha` `brave` `deepseek` `exa` `generic` `google` `jina` `none` `searxng` `serper` `tavily` `zhipu`

常用几家：

| Provider | 价格 | 说明 |
|---|---|---|
| `deepseek` | 5 元/1000 次 | 官方独立接口（`/v1/tools/search` + `/v1/tools/fetch`），与对话同 key |
| `tavily` | 免费 1000 次/月 | 海外内容好 |
| `bocha` | 0.036 元/次 | 国内网页全 |
| `searxng` | 免费 | 需自建实例 |
| `generic` | 取决于对方 | 任意 HTTP 端点（自建服务 / 本地桥接） |
| `none` | — | 关闭搜索 |

各家字段差异（url vs link、title vs name）已在实现里适配，不用为每家准备不同的端点格式。

**默认 deepseek**：出厂即配好协议，只差一把 key。填上就能用（见下）。

### 3.6 两种搜索：服务端工具 vs 本地工具

搜索有两条**互斥**的实现路径，由「对话提供方是不是 DeepSeek 官方端点」决定走哪条。

| | 服务端联网工具 | 本地工具 |
|---|---|---|
| 开关 | `capability.chat.deepseek_web_tools`（默认 true） | `capability.search.provider` + `api_key` |
| 生效条件 | 开关开 **且** 端点是 DeepSeek 官方 | `provider != "none"` **且** api_key 解析出非空值 |
| 实际执行 | DeepSeek 服务端（`deepseek_web_search` / `deepseek_read_url`） | 本机进程（`web_search` / `web_fetch` 工具） |
| 行为 | **边思考边搜**：多步推理中自主搜索 / 开网页 / 页内查找，约 20 次调用 | 模型自己决定何时调工具，一次一查 |
| 引用 | 服务端直接引用原文 | 工具返回 title/url/snippet，模型自己组织 |
| 计费 | 按次计费（走模型 token 账） | 搜索接口按次计费（DeepSeek 官方 5 元/1000 次） |
| 本地工具注册 | **不注册**（`web_search` / `web_fetch` 会被注销） | 注册 |

**判据是 `base_url`，不是 `provider` 名字**：控制面在用户选「DeepSeek 官方」时
会把 `capability.chat.provider` 写成接入方式 `"openai"`，所以 provider 字段几乎
永远是 `openai`。真正判据是 `fish::capability::is_deepseek_endpoint()`：
`provider == "deepseek"`，或 `base_url` 含 `api.deepseek.com` / `deepseek`。

**互斥的实现方式**：`fish::capability::server_web_tools_active(cfg)` 是**唯一判据来源**，
capability（是否注入服务端工具）和 `main.cpp`（是否注册本地工具）都用它。

> 历史上两边各判各的：capability 只看开关，`main.cpp` 也只看开关，
> 但「开关开 + 端点非官方」（WorkBuddy 桥接部署）时两边结论还不一致，
> 结果本地 `web_search`/`web_fetch` 被注销、服务端工具又没注入——
> **两个搜索工具同时消失，搜索能力凭空蒸掉且没有任何报错**。
> 现在判据统一成「开关开 **且** 端点是官方」，非官方端点时本地工具照常注册。

### 3.7 怎么选

| 场景 | 用哪种 | 怎么配 |
|---|---|---|
| 对话走 DeepSeek 官方端点 | **服务端工具**（默认） | 什么都不用配，开关默认 true |
| 对话走 WorkBuddy 桥接 / 其它 OpenAI 兼容端点 | **本地工具** | `capability.search.provider = "deepseek"` + 填 `DEEPSEEK_API_KEY` |
| 想要更便宜的搜索（免费额度 / 自建实例） | **本地工具** | `provider` 换 `tavily` / `searxng` / `bocha` … + 对应的 api_key |
| 对话走官方，但想省服务端搜索的按次费用 | **本地工具** | `capability.chat.deepseek_web_tools = false` + 配 `capability.search` |

**开关闭关都不静默**，三个地方都会如实说：

1. **启动日志**：开关开着但端点非官方时打
   `info / capability / deepseek_web_tools_inactive`，带 `provider`、`base_url`
   和一句 `hint: 端点非 DeepSeek 官方，服务端联网工具不生效，使用本地搜索`。
2. **控制面**：`GET /v1/config` 下发三个字段，GUI 据此如实显示当前走哪条路
   （见 [CONTROL_API.md](../CONTROL_API.md)）。
3. **本地搜索没开时**：`capability.search.api_key` 解析为空 → 启动打
   `warn / config / env_var_missing / var=DEEPSEEK_API_KEY`；
   控制面 `capability.search.key_ready = false`，GUI 的搜索 API Key 输入框会出现，
   填进去保存即生效。

**开关本身不改变搜索是否可用**：端点非官方时它无效，但本地搜索照常工作；
端点是官方时它才把搜索从「本地工具」切到「服务端工具」。


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
# max_tokens / temperature 不写 = 不发送 = 用上游默认值
# reasoning_effort 锁死 high（产品要求），控制面也拒绝修改

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
provider = "deepseek"                 # 默认已配好协议，只差一把 key
api_key  = "${DEEPSEEK_API_KEY}"      # 没填 = 搜索不启用（但会明确提示）
max_results = 5
```

各能力的 provider 清单（**只列真托管 DeepSeek 的厂商**）：

| 能力 | 可选 provider |
|---|---|
| 对话 | `deepseek` `workbuddy` |
| 向量 | `ark` `dashscope` `qianfan` `siliconflow` `workbuddy` |
| 语音识别 | `dashscope` `deepseek` `siliconflow` `volcengine` |
| 语音合成 | `dashscope` `deepseek` `edge` `siliconflow` `volcengine` |
| 搜索 | `bocha` `brave` `deepseek` `exa` `generic` `google` `jina` `none` `searxng` `serper` `tavily` `zhipu` |
| 生图 | `bridge` `openai` `siliconflow` |
| 文档 | `deepseek` `none`（独立 `fish.doc` 层，见 [doc.md](doc.md)） |

> 底层模型锁死 DeepSeek，所以「能选哪家」等价于「哪家真能出 DeepSeek」——
> 列一家不托管 DeepSeek 的厂商，用户选了也只会拿到别的模型，
> 等于绕过了锁定，所以干脆不列。

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

实现上不是注册表，而是一份**白名单字符串**（`chat_provider_list()` 等），
`make_xxx` 按同一字符串 split 校验：

```cpp
const char* chat_provider_list() {
  return "deepseek,workbuddy";
}
const char* embedding_provider_list() {
  return "ark,dashscope,qianfan,siliconflow,workbuddy";
}
const char* stt_provider_list() {
  return "dashscope,deepseek,siliconflow,volcengine";
}
const char* tts_provider_list() {
  return "dashscope,deepseek,edge,siliconflow,volcengine";
}
const char* search_provider_list() {
  return "bocha,brave,deepseek,exa,generic,google,jina,none,searxng,serper,tavily,zhipu";
}
const char* image_provider_list() { return "bridge,openai,siliconflow"; }
```

> **清单只有一份**，`make_search` 的白名单直接 split `search_provider_list()`。
> 曾经三处各写一份，结果不一致——要么「配置里写了 provider 就启动失败」，
> 要么「分支写好了但永远走不到」。

**默认值自动补全**：配置里只写 `provider`，`base_url` 和 `model` 用默认，
减少必填项。显式写了就用显式的。

### 5.2 未知 provider

启动时报错退出，并列出支持的 provider：

```
unknown chat provider 'claude2'. supported: deepseek,workbuddy
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
| embedding | 无向量，只剩 LIKE 关键词路兜底 | 记忆没那么准 |
| stt | 占位文本 `[语音转写失败]` | 知道没听清 |
| tts | 只发文本 | 没有语音 |
| search | 跳过搜索直接回答 | 回答可能不够新（启动日志会说搜索没开） |
| document | 占位文本「[收到文件：xxx，解析失败]」 | 知道收到了文件，读不到正文 |

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
| 搜索 | `deepseek`（默认）/ `tavily` / `searxng` | 否 | DeepSeek 5 元/1000 次；tavily 1000 次/月内 0 |

**最小可用组合**：
- 一个 DeepSeek key（付费）—— 对话、搜索、文档解析共用这一把
- 一个硅基流动 key（免费，需实名）
- Edge-TTS 无需 key

**两把 key，全部能力可用。**

> 搜索默认已配好 `provider = "deepseek"`，DeepSeek key 填进 `.env.local` 的
> `DEEPSEEK_API_KEY` 即生效（同一把 key 也供对话和文档解析复用）。
> 不想为搜索付费就换 `tavily`（每月 1000 次免费）或自建 `searxng`。
> 详见 [3.6 两种搜索](#36-两种搜索服务端工具-vs-本地工具)。

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
| embedding 失败降级 | 只剩 LIKE 关键词路，对话仍进行 |
| stt 失败降级 | 占位文本，消息仍发出 |
| tts 失败降级 | 只发文本 |
| 429 退避序列 | Mock 连续 429，验证退避 |

**均 Mock HTTP，不打真实 API。**

---

## 12. WorkBuddy 接入（桥接层模式）

WorkBuddy 桌面端的模型通过**独立桥接进程**接入，不在本模块内实现。
详见 [bridge/README.md](../bridge/README.md)。

配置：

```toml
[capability.chat]
provider = "workbuddy"                    # 桥接层专用 provider
base_url = "http://127.0.0.1:18091/v1"   # 桥接层地址
api_key  = "bridge-local"                # 桥接层不校验 key，非空即可
model    = "deep-model"
timeout_ms = 90000
```

`provider = "workbuddy"` 与 `provider = "openai"` 的区别：
`workbuddy` 会带上 DeepSeek 服务端联网工具相关的处理，
并接受桥接层的抽象模型名（`fast-model` / `balanced-model` / `deep-model` / `auto`）。

> **桥接部署下的搜索走本地**：`base_url` 指向 `127.0.0.1:18091`，
> 不是 DeepSeek 官方端点，所以 `capability.chat.deepseek_web_tools` 开关无效
> （启动日志会打 `deepseek_web_tools_inactive` 说明这一点），
> 搜索照常走本地 `web_search` / `web_fetch`——
> 只要 `capability.search.provider != "none"` 且 key 解析出非空值。
> 见 [3.6 两种搜索](#36-两种搜索服务端工具-vs-本地工具)。

> **桥接层启动必须带 `--cred-file`**，否则 GUI「扫码登录 WorkBuddy」
> 写入的凭据（`data/workbuddy.json`）不会被桥读取，登录等于白发。
> 见 [deploy/README.md](../../deploy/README.md)。

**为什么不在 capability 里直接实现**：

1. 凭据管理与刷新是独立关注点，本体不应持有第三方凭据
2. 上游强制流式（SSE），与"一次请求一次响应"的接口不匹配，转换属于桥接职责
3. 可替换性：停掉桥接、provider 改回 deepseek，本体零改动

这是「职责分离」在**进程边界**上的体现。
