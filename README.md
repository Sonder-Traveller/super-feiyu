# super-feiyu

C++20 实现的「大肥鱼」机器人内核 —— DeepSeek 拟人化鲸鱼娘的被动对话系统。

**目标环境**：2 核 1G Ubuntu。本机零推理，全部能力走远程 API，
只做协议转发与数据落盘。

> 状态：**可构建、可运行、测试全绿**。用 clang C++20 模块实现。

---

## 为什么自己做

社区主流方案 **AstrBot + NapCat** 在目标机器上不可行：

| 组件 | 空闲内存 | 进程数 |
|---|---|---|
| NapCat | 532 MiB | 86 |
| AstrBot | 323 MiB | 7 |
| 合计 | **≈ 855 MiB** | 93 |

1G 机器系统本身占 100–150 MB，**剩余不到 900 MB，必 OOM**。
根本原因是 NapCat 必须跑完整 NTQQ 客户端。

直连官方 Bot API 就绕过了这个矛盾。详见 [ADR-001](docs/decisions/001-no-napcat.md)。

| 维度 | AstrBot + NapCat | super-feiyu |
|---|---|---|
| 语言 | Python | **C++20（模块）** |
| 内存 | 855 MB | 目标 < 100 MB |
| 依赖 | Python + Docker + Xvfb | 单个二进制 |
| 职责边界 | 插件层交杂 | 编译期强制分层 |
| 封号风险 | 有（非官方协议） | 零（官方协议） |

---

## 架构：七个模块，单向依赖

```
        runtime (main.cpp)
               │
        orchestrator ── 唯一持有业务决策
               │
   ┌──────┬────┴────┬──────────┬─────────┐
transport  capability  memory  config   log
   │           │         │
 QQ官方/小号  DeepSeek   L2 短时
 微信/本地    硅基流动   L3 长时（旁路文件 + 索引）
             Edge-TTS
```

全部用 C++20 模块（`export module`），**没有头文件**。

| 模块 | 职责 |
|---|---|
| `fish.core` | 领域类型、`Result`、七类接口契约 |
| `fish.log` | 结构化 JSON 日志 |
| `fish.config` | TOML 子集 + 环境变量展开 |
| `fish.memory` | **三层记忆**（本项目的核心） |
| `fish.capability` | 远程能力，多 provider 统一接口 |
| `fish.transport` | QQ 官方 / QQ 小号 / 微信 / 本地 |
| `fish.orchestrator` | 流水线、被动策略、预算、去重 |

---

## 三个关键设计

### 1. 记忆：三层，短时与长时分离

类比人脑：**短时记忆在工作区，长时记忆在外存，中间有巩固与遗忘**。

| 层 | 存储 | 作用 |
|---|---|---|
| L2 短时 | 内存环形缓冲（20 轮） | 「刚才在聊什么」 |
| L3a 情景 | `notes/episodes/YYYY-MM.md` | 历史对话摘要 |
| L3b 语义 | `notes/facts.md` | 提炼出的事实 |
| L3c 程序 | `notes/traits.md` | 行为偏好（≥3 次证据才确立） |

**关键决策：旁路 Markdown 文件是权威源，SQLite 只是可重建的索引。**

这样设计的好处：
- 记忆**人可读**，出问题能直接看、能手改
- 可 `git` 版本化
- **SQLite 删掉能从文件重建**（已实测验证）
- 换机器拷 `data/` 即走

巩固：每 20 轮一次独立 LLM 调用，把对话提炼成结构化 JSON。
**巩固失败不阻断对话**，记忆会恢复、下轮重试。

遗忘：权重按 30 天半衰期衰减，常用记忆因召回刷新而不衰减。
冲突事实不删除，标记 `superseded_by` 保留历史。

检索：向量 + 关键词双路，**RRF 融合**（无需调权重）。

详见 [docs/modules/memory.md](docs/modules/memory.md)。

### 2. 成本：前缀缓存是架构约束

DeepSeek 输入价格：缓存命中 **0.02 元/百万**，未命中 **1 元/百万**，**差 50 倍**。

| 场景 | 月成本 |
|---|---|
| 缓存命中 | 0.3 元 |
| 缓存未命中 | 15 元 |

失效是**静默的** —— 不报错，只让账单变贵。所以：

- `IPersona::stable_prefix()` 返回 `const std::string`，想拼东西做不到
- `IChatModel::Request` 把 `system_prompt` 与 `history` 分离，强制置前
- 运行时监控 `cached_tokens / prompt_tokens`

实测已确认 `cached_tokens` 能正确采集（967/969）。

### 3. 能力：一套接口，任意厂商

```
上层 ──▶ 统一接口 ──▶ Provider 工厂 ──▶ 协议适配器（OpenAI / Anthropic）
```

换厂商只改配置两行：

```toml
provider = "groq"
base_url = "https://api.groq.com/openai/v1"
```

各能力的免费预设：

| 能力 | 默认 | 备选 |
|---|---|---|
| 对话 | `deepseek/deepseek-flash` | openai / anthropic / siliconflow / ollama |
| 向量 | `siliconflow/BAAI/bge-m3`（免费） | openai / ollama |
| 语音识别 | `siliconflow/SenseVoiceSmall`（免费） | groq / openai / local |
| 语音合成 | `edge`（**无需 key**） | openai / siliconflow |

**也可以通过桥接层用 WorkBuddy**：见 [bridge/README.md](bridge/README.md)。
桥接是独立进程，本体零改动，只需把 provider 改成 `openai` 指向它。
| 搜索 | `none` | deepseek / tavily / bocha |

**注意：DeepSeek 没有 embeddings 接口**，向量必须用别家。
详见 [ADR-008](docs/decisions/008-two-vendor-keys.md)。

---

## 关键限制（平台硬约束）

| 限制 | 数值 |
|---|---|
| 被动回复次数 | 单聊 4 条 / 群聊 5 条 |
| 被动回复窗口 | 单聊 60 min / 群聊 5 min |
| 主动消息 | **不可用**（2025-04 停用） |

已内化为 `ChannelCapability.max_replies_per_message` 与 `ReplyBudget`。

**QQ 小号通道绕过了这些限制**：走 OneBot v11（NapCat/Lagrange），
能以真实 QQ 号收发，无配额。

---

## 通道对比

| | QQ 官方 | QQ 小号（OneBot） | 微信 iLink |
|---|---|---|---|
| 身份 | bot | 真实 QQ 号 | 微信单聊 |
| 群聊 | ✅ @ 触发 | ✅ 完整 | ❌ |
| 回复配额 | 4/5 条 | 无 | 无 |
| 富媒体 | ✅（需上传） | ✅ | ❌ 仅文本 |
| 封号风险 | 零 | 有（需小号） | 中 |
| 内存 | 低 | 需 NapCat（532MB） | 低 |

**建议**：2C1G 用官方通道；有 2G 以上再考虑小号。

---

## 构建

依赖：clang 16+（需模块支持）、libcurl、sqlite3、nlohmann/json。

```bash
# 一次性准备依赖（无 root 环境）
apt-get download libcurl4-openssl-dev libcurl4t64 libsqlite3-dev
for f in *.deb; do dpkg -x "$f" ~/local; done
curl -fsSL https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp \
  -o ~/local/include/nlohmann/json.hpp  # 需先 mkdir -p

# 构建
./build.sh

# 全量验证（构建 + 单测 + 端到端 + 真实 HTTP 链路）
./verify.sh

# 部署到 ~/super-feiyu
./deploy.sh
```

产物：
- `build/super-feiyu` — 主程序
- `build/test_all` — 12 组单元测试
- `build/test_e2e` — 端到端记忆闭环

### 运行

```bash
cp config.example.toml config.toml   # 填 key
export DEEPSEEK_API_KEY=sk-xxx
export SILICONFLOW_API_KEY=sk-yyy    # 向量/语音识别
./build/super-feiyu --config config.toml
```

模式：
- `--self-test` — 桩模式全链路自检（不联网）
- `--live-test` — 走真实 provider 的链路自检
- `--stub` — 强制桩模式

---

## 测试

| 套件 | 覆盖 |
|---|---|
| `test_all` | 被动红线、预算、去重、三层记忆、巩固、遗忘、索引重建、OneBot 解析 |
| `test_e2e` | 多轮对话 → 巩固 → 旁路文件 → 索引召回 → **删索引重建** |

端到端测试配合 `tests/mock_server.py`（本地模拟 API），
**不依赖真实 key，也不产生费用**。

实测 E2E 输出：
```
巩固触发: episodes=1 facts=2 traits=0
索引条目数: 3
索引删除后重建成功
重建后条目数: 3      ← 证明旁路文件是真正的权威源
E2E PASSED
```

---

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 总览：分层、数据流、并发、成本、错误模型 |
| [docs/modules/memory.md](docs/modules/memory.md) | **三层记忆**（重点） |
| [docs/modules/capability.md](docs/modules/capability.md) | 多 provider 统一接口 |
| [docs/modules/transport.md](docs/modules/transport.md) | 通道实现 |
| [docs/modules/orchestrator.md](docs/modules/orchestrator.md) | 流水线与策略 |
| [docs/modules/persona.md](docs/modules/persona.md) | 分层提示词 |
| [docs/modules/store.md](docs/modules/store.md) | 索引存储 |
| [docs/decisions/](docs/decisions/) | 8 条关键取舍（ADR） |

---

## 目录

```
src/
  fish.core.cppm          领域类型与接口
  fish.log.cppm           结构化日志
  fish.config.cppm        配置解析
  fish.memory.cppm        三层记忆（接口 + private 实现）
  fish.capability.cppm    远程能力 + provider 工厂
  fish.transport.cppm     四类通道
  fish.orchestrator.cppm  流水线
  main.cpp                runtime 装配
tests/
  test_all.cpp            单元测试
  test_e2e.cpp            端到端
  mock_server.py          本地模拟 API
bridge/
  workbuddy_bridge.py    WorkBuddy 桥接层（独立进程）
  README.md              桥接层说明
persona/
  persona.md              人格正文
  rules.md                行为红线
docs/                     设计文档
build.sh verify.sh deploy.sh
```

## 许可

MIT

