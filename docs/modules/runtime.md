# runtime 模块设计

> 职责：配置加载、依赖组装、生命周期管理、日志与指标。
> **不含业务逻辑**，只负责"把零件拼起来并活着"。

---

## 1. 定位

runtime 是唯一的**装配点**。它是唯一同时看到所有实现的模块，
因此也是唯一允许 `#include` 各实现头文件的地方。

```
runtime
  ├─ 读配置 → 校验 → 失败即退出
  ├─ 按配置构造各实现（通道/能力/存储）
  ├─ 注入 orchestrator::Pipeline::Deps
  ├─ 启动线程
  ├─ 处理信号
  └─ 优雅退出
```

---

## 2. 配置

### 2.1 文件结构

`config.toml`：

```toml
[log]
level = "info"
format = "json"

[channel.qq]
enabled = true
app_id = "102xxxxx"
app_secret = "${QQ_APP_SECRET}"    # 支持环境变量引用
intents = ["group_at", "c2c"]

[channel.wechat]
enabled = false
# 微信扫码后 token 落 data/wechat_token.json

[capability.chat]
provider = "deepseek"
base_url = "https://api.deepseek.com"
api_key = "${DEEPSEEK_API_KEY}"
model = "deepseek-flash"
# max_tokens / temperature 不写 = 不发送 = 用上游默认值
timeout_ms = 30000

[capability.embedding]
provider = "siliconflow"
base_url = "https://api.siliconflow.cn/v1"
api_key = "${SILICONFLOW_API_KEY}"
model = "BAAI/bge-m3"
dimension = 1024

[capability.stt]
enabled = true
base_url = "https://api.siliconflow.cn/v1"
model = "FunAudioLLM/SenseVoiceSmall"

[capability.tts]
enabled = true
provider = "edge"
voice = "zh-CN-XiaoyiNeural"
# 语音门控：群里永不发语音
group_voice = false

[store]
type = "sqlite"
path = "data/memory.db"
max_messages_per_conv = 2000
max_db_mb = 200

[orchestrator]
history_window = 8
recall_top_k = 5
dedup_capacity = 1024
shard_count = 64
total_timeout_ms = 15000

[persona]
persona_file = "persona/persona.md"
rules_file = "persona/rules.md"
```

### 2.2 密钥处理

**铁律**：密钥永不进配置文件、永不进仓库。

三种来源，优先级从高到低：

1. 环境变量（`${VAR}` 引用）
2. `.env` 文件（本地开发）
3. 直接写值（**仅限非敏感配置**）

启动时检查日志里的密钥必须脱敏：

```
api_key = "sk-1234****cdef"
```

### 2.3 校验策略

**启动即校验，失败即退出**。不允许"能跑起来再说"。

| 检查项 | 失败行为 |
|---|---|
| 必填项缺失 | 退出，打印缺失项 |
| 密钥未设置 | 退出，提示设置哪个环境变量 |
| 人格文件不存在 | 退出 |
| 人格文件为空 | 退出 |
| 端口被占用 | 退出 |
| 数据目录不可写 | 退出 |
| 模型名不在白名单 | 警告（不退出，允许自定义） |

**为什么人格为空要退出**：空人格的机器人会用"没有性格"的方式说话，
作者往往以为只是"今天有点冷淡"，实际上提示词根本没加载。
**静默失败比崩溃危险得多。**

---

## 3. 依赖组装

### 3.1 构造顺序

```
1. 日志
2. 配置
3. store（最先，后续都可能用到）
4. capability（HTTP 客户端需先初始化）
5. persona（加载文件）
6. budget / dedup
7. channel（最后，因为要开始连网）
8. pipeline（注入以上全部）
```

### 3.2 依赖注入方式

用裸指针 + 生命周期由 runtime 保证（不用 `shared_ptr`）：

```cpp
struct Runtime {
  std::unique_ptr<IMemoryStore> store;
  std::unique_ptr<IChatModel> chat;
  std::unique_ptr<IEmbeddingModel> embedding;
  std::unique_ptr<ISpeechToText> stt;
  std::unique_ptr<ITextToSpeech> tts;
  std::unique_ptr<IPersona> persona;
  std::unique_ptr<IReplyBudget> budget;
  std::vector<std::unique_ptr<IChannel>> channels;
  std::unique_ptr<Pipeline> pipeline;
};
```

**为什么不用 `shared_ptr`**：所有权清晰，runtime 唯一持有，
Pipeline 只拿裸指针观察。减少引用计数的原子操作开销，
也避免循环引用排查。

---

## 4. 线程模型

```
main        ── 等待信号，协调退出
channel[i]  ── 每通道一个，阻塞 poll
worker[2]   ── 从队列取任务，执行 Pipeline
timer       ── 预算清扫、去重 GC、指标刷新
```

### 4.1 工作队列

```cpp
struct WorkItem { IChannel* channel; InboundMessage msg; };
BoundedQueue<WorkItem> queue_{128};
```

**有界**是硬要求。满载时的策略：

1. 私聊优先入队
2. 群聊直接丢弃，`metric: dropped_backpressure`

**绝不无界排队** —— 1G 机器上积压 = OOM。

### 4.2 优雅退出

```
SIGTERM/SIGINT
  → 置 stop flag
  → channel 线程从 poll 返回（超时或主动关闭）
  → 工作线程处理完当前任务后退出
  → timer 线程退出
  → 关闭 store（确保 WAL checkpoint）
  → 进程退出
```

**超时保护**：10 s 内没退完就强制退出，避免 systemd 一直等。

**必须 checkpoint WAL**：否则异常退出后 WAL 文件可能很大，
下次启动要回放很久。

---

## 5. 日志

### 5.1 格式

单行 JSON：

```json
{"ts":"2026-11-12T23:50:01.123+08:00","level":"info","module":"orchestrator",
 "event":"reply_sent","conv":"g123","latency_ms":2345,"tokens":{"prompt":4200,"cached":3800,"output":85}}
```

### 5.2 纪律

| 规则 | 理由 |
|---|---|
| **不打消息正文** | 隐私 |
| 只打长度和哈希 | 够排查 |
| 密钥永远脱敏 | 安全 |
| `conversation_id` 必带 | 追踪单会话 |
| 错误必带 `code` 和 `module` | 聚合分析 |

### 5.3 级别使用

| 级别 | 场景 |
|---|---|
| `error` | 需要人介入（密钥失效、存储损坏） |
| `warn` | 降级发生（上游超时、TTS 失败） |
| `info` | 正常事件（消息处理、回复发送） |
| `debug` | 开发排查（默认关闭） |

**不要用 `info` 打每条心跳**，日志会爆。

---

## 6. 指标

### 6.1 进程内聚合

不引入 Prometheus 客户端库（体积与复杂度不划算），
用简单计数器 + 定期打印：

```cpp
struct Metrics {
  std::atomic<uint64_t> msg_in_total{0};
  std::atomic<uint64_t> reply_total{0};
  std::atomic<uint64_t> silent_total{0};
  std::atomic<uint64_t> error_total{0};
  std::atomic<uint64_t> degrade_total{0};
  std::atomic<uint64_t> prompt_tokens{0};
  std::atomic<uint64_t> cached_tokens{0};
  std::atomic<uint64_t> output_tokens{0};
};
```

每 5 分钟打印一次摘要：

```json
{"event":"metrics","msg_in":42,"reply":38,"silent":4,
 "cache_hit_ratio":0.91,"est_cost_cny":0.12,"rss_mb":48}
```

### 6.2 关键指标：缓存命中率

```
cache_hit_ratio = cached_tokens / prompt_tokens
```

**这是最重要的健康指标**。低于 0.7 说明前缀缓存被破坏，
账单会悄悄翻 50 倍。必须监控。

### 6.3 内存自监控

```cpp
rss_mb = read /proc/self/status VmRSS
```

超过阈值（如 200 MB）时告警。1G 机器上这是保命机制。

---

## 7. systemd 单元

```ini
[Unit]
Description=super-feiyu
After=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/super-feiyu
ExecStart=/opt/super-feiyu/super-feiyu --config /opt/super-feiyu/config.toml
EnvironmentFile=/opt/super-feiyu/.env
Restart=always
RestartSec=5
MemoryMax=512M
LimitNOFILE=4096

[Install]
WantedBy=multi-user.target
```

`MemoryMax=512M` 是兜底，目标是实际 < 100 MB。
触发限制说明有泄漏，需要排查。

---

## 8. 待定事项

| 事项 | 选项 | 倾向 |
|---|---|---|
| 配置格式 | TOML / YAML / JSON | **TOML**（易读，C++ 库成熟） |
| 配置库 | toml11 / toml++ | **toml++**（单头，无依赖） |
| 日志库 | spdlog / 自写 | **自写**（需求简单，少依赖） |
| 热重载 | 支持 / 重启生效 | 重启（避免运行中状态不一致） |
| 是否暴露 HTTP 指标端点 | 是 / 否 | 否（先日志，需要再加） |
| 崩溃时是否 dump | 是 / 否 | 否（内存小，dump 反而麻烦） |

---

## 9. 测试要点

| 用例 | 断言 |
|---|---|
| 缺失必填配置 | 退出码非 0，错误信息含字段名 |
| 缺失密钥 | 退出，提示环境变量名 |
| 人格文件缺失 | 退出 |
| 人格文件为空 | 退出 |
| 密钥脱敏 | 日志里不出现完整密钥 |
| 信号退出 | SIGTERM 后 10 s 内进程结束 |
| WAL checkpoint | 退出后 `-wal` 文件为空或不存在 |
| 背压丢群聊保私聊 | 队列满时私聊仍被处理 |
| 指标打印 | 定时任务确实触发 |

**配置校验的测试最有价值**：能防止"部署了但静默不工作"这类最难排查的问题。
