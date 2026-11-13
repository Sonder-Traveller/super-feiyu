# super-feiyu

C++20 实现的「大肥鱼」机器人内核 —— DeepSeek 拟人化鲸鱼娘的被动对话系统。

**目标环境**：2 核 1G Ubuntu。本机零推理，全部能力走远程 API，
只做协议转发与数据落盘。实测常驻内存 **15–30 MB**。

---

## 状态

| 项 | 状态 |
|---|---|
| 构建 | ✅ clang 20 个 C++20 模块，一次通过 |
| 测试 | ✅ 306 项断言（5 个套件，全绿） |
| 通道 | ✅ QQ 官方 WS / QQ 小号 OneBot WS / 微信 / 本地 |
| 记忆 | ✅ 三层 + 巩固 + 检索 + 索引重建 |
| 部署 | ✅ systemd 用户服务，已实测启动/停止/重连 |
| 真实 API | ✅ 硅基流动免费模型（embedding/STT）已验证 |

---

## 为什么自己做

社区主流方案 **AstrBot + NapCat** 在 1G 机器上不可行：

| 组件 | 空闲内存 |
|---|---|
| NapCat | 532 MiB |
| AstrBot | 323 MiB |
| 合计 | **≈ 855 MiB** |

系统本身占 100–150 MB，**剩余不到 900 MB，必 OOM**。
根本原因是 NapCat 必须跑完整 NTQQ 客户端。

本项目直连官方 Bot API / OneBot，**无协议端进程**。

| 维度 | AstrBot + NapCat | super-feiyu |
|---|---|---|
| 内存 | 855 MB | **15–30 MB** |
| 依赖 | Python + Docker + Xvfb | 单个二进制 |
| 职责边界 | 插件层交杂 | 编译期强制分层 |
| 封号风险 | 有（非官方协议） | 官方通道零风险 |

---

## 架构：20 个 C++20 模块

```
              main.cpp (runtime)
                     │
              dispatcher ── 线程池 + 会话隔离
                     │
              orchestrator ── 唯一持有业务决策
                     │
    ┌──────┬─────────┼─────────┬──────┬──────┬──────┐
 channels  capability  memory  persona config  log
    │          │          │
  qq_ws    DeepSeek     L2 短时
 onebot_ws 硅基流动     L3 长时（旁路文件 + 索引）
    │      Edge-TTS
    ws (libcurl WebSocket 底层)
```

**全部是模块，没有头文件。** 依赖方向单向，靠编译期保证。

| 模块 | 职责 |
|---|---|
| `fish.core` | 领域类型、`Result`、十类接口 |
| `fish.log` | 结构化 JSON 日志 |
| `fish.config` | TOML + 环境变量展开 |
| `fish.memory` | **三层记忆**（核心） |
| `fish.capability` | 多 provider 统一接口 + 熔断 |
| `fish.probe` | 上游能力探测与缓存 |
| `fish.media` | 媒体去重缓存（内容哈希） |
| `fish.image` | 立绘库（manifest 权威 + 目录扫描） |
| `fish.meme` | 热梗库（本地优先 + 命中计数） |
| `fish.session` | 会话注册表 + 每会话互斥 |
| `fish.persona` | 人格分层拼装（稳定前缀 / 动态后缀） |
| `fish.api` | 控制面 HTTP API（人格管理，带鉴权） |
| `fish.tool` | 工具注册表（记忆/图片/搜索/抓取/梗） |
| `fish.guard` | 输入注入防护 + 输出脱敏 + 限流 |
| `fish.ws` | libcurl WebSocket 客户端 + 自动重连 |
| `fish.qq_ws` | QQ 网关协议栈（op 码/心跳/RESUME） |
| `fish.onebot_ws` | OneBot v11 事件解析 |
| `fish.channels` | 四个通道实现（含富媒体上传） |
| `fish.dispatcher` | 线程池调度，通道线程只投递 |
| `fish.orchestrator` | 流水线、被动策略、预算、去重 |

> 模块必须按依赖顺序编译，见 `build.sh` 里的 `MODULES` 数组。

---

## 三个关键设计

### 1. 记忆：三层，短时与长时分离

| 层 | 存储 | 作用 |
|---|---|---|
| L2 短时 | 内存原文缓冲（窗口的 50% 为上限） | 「刚才在聊什么」 |
| L3a 情景 | `notes/<kind>/<conv>/episodes/YYYY-MM.md` | 历史对话摘要 |
| L3b 语义 | `notes/<kind>/<conv>/facts.md` | 提炼出的事实 |
| L3c 程序 | `notes/<kind>/<conv>/traits.md` | 行为偏好（≥3 次证据） |

**旁路 Markdown 是权威源，SQLite 只是可重建索引。**

实测：删掉 `index.db` 后能从文件重建出全部条目。

巩固用独立 LLM 调用，**失败不阻断对话**（记忆恢复、下轮重试）。
去重用内容哈希做稳定 id，**连跑多次不产生重复**。

**记忆只增不减，没有任何条数或时间上限。** 预算按 token 算：
`模型窗口 × raw_context_ratio`，其中 `raw_context_ratio` **硬上限 50%**
（默认仍是 131072 × 0.40，配置写更高也只按 50% 算）。在预算内全部原文
进上下文；超了就 `compact()` —— 最早的部分压成滚动摘要，原文落盘到
`episodes/` 并建索引，模型用 `search()` 自己检索。**不删任何一条。**

详见 [docs/modules/memory.md](docs/modules/memory.md)。

### 2. 成本：前缀缓存是架构约束

缓存命中 0.02 元/百万 vs 未命中 1 元/百万，**差 50 倍**。

- `IPersona::stable_prefix()` 返回 `const std::string`，想拼东西做不到
- `IChatModel::Request` 把 `system_prompt` 与 `history` 分离
- 运行时输出 `cached_tokens`（实测 967/970，命中率 99%）

### 3. 能力：一套接口，任意厂商

换厂商只改配置两行。免费组合：

| 能力 | 默认 | 费用 |
|---|---|---|
| 对话 | `deepseek/deepseek-flash` | 按 token |
| 向量 | `siliconflow/BAAI/bge-m3` | **免费** |
| 语音识别 | `siliconflow/SenseVoiceSmall` | **免费** |
| 语音合成 | `edge` | **免费，无需 key** |
| 搜索 | `none` | — |

> DeepSeek 没有 embeddings 接口，向量必须另配。见 [ADR-008](docs/decisions/008-two-vendor-keys.md)。

---

## 通道对比

| | QQ 官方 | QQ 小号（OneBot） | 微信 iLink |
|---|---|---|---|
| 身份 | bot | 真实 QQ 号 | 微信单聊 |
| 群聊 | ✅ @ 触发 | ✅ 完整 | ❌ |
| 回复配额 | 单聊 4 / 群 5 | 无 | 无 |
| 富媒体 | ✅ 自动上传 | ✅ CQ 码 | ❌ 仅文本 |
| 封号风险 | 零 | 有（用小号） | 中 |
| 断线重连 | ✅ | ✅ | ✅ |

QQ 小号绕过了官方配额限制，但需另跑 NapCat/Lagrange（约 532MB），
**1G 机器只建议用官方通道**。

---

## 快速开始

```bash
# 依赖（无 root）
mkdir -p ~/local && cd /tmp
apt-get download libcurl4-openssl-dev libcurl4t64 libsqlite3-dev libsqlite3-0
for f in *.deb; do dpkg -x "$f" ~/local; done
mkdir -p ~/local/include/nlohmann
curl -fsSL https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp \
  -o ~/local/include/nlohmann/json.hpp

# 部署（含构建 + systemd）
./deploy/install.sh

# 填配置与密钥
vim ~/super-feiyu/config.toml    # 启用通道
vim ~/super-feiyu/.env           # 填 key

# 启动
systemctl --user start super-feiyu
journalctl --user -u super-feiyu -f
```

详见 [deploy/README.md](deploy/README.md)。

---

## 验证

```bash
./verify.sh              # 全量：构建 + 5 类测试 + 真实 API
./verify.sh --no-network # 跳过真实网络
```

实测输出：

```
✓ 构建（20 个模块）
✓ 单元测试        (173 项)
✓ 协议测试        ( 24 项)
✓ 生命周期测试    ( 20 项)
✓ 记忆测试        ( 83 项)
✓ 端到端测试      (  6 项)
✓ 缺配置拒绝启动
✓ 真实 embedding（bge-m3，免费模型）
```

合计 **306 项断言，全绿**。逐套件统计：`bash tools/count_pass.sh`
（这个脚本直接跑二进制数 `[PASS]`，不数源码里的 `CHECK`）。

---

## 关键限制（平台硬约束）

| 限制 | 数值 |
|---|---|
| 被动回复次数 | 单聊 4 / 群 5 |
| 被动回复窗口 | 单聊 60 min / 群 5 min |
| 主动消息 | **不可用**（2025-04 停用） |

已内化为 `ChannelCapability` 与 `ReplyBudget`。

---

## WorkBuddy 桥接层（可选）

把本机已登录的 WorkBuddy 接入，**独立进程，本体零改动**：

```bash
python3 bridge/workbuddy_bridge.py --port 18081
```

```toml
[capability.chat]
provider = "openai"
base_url = "http://127.0.0.1:18081/v1"
```

详见 [bridge/README.md](bridge/README.md)。

---

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 总览 |
| [docs/modules/memory.md](docs/modules/memory.md) | **三层记忆**（重点） |
| [docs/modules/capability.md](docs/modules/capability.md) | 多 provider |
| [docs/modules/transport.md](docs/modules/transport.md) | 通道与协议 |
| [docs/modules/orchestrator.md](docs/modules/orchestrator.md) | 流水线 |
| [deploy/README.md](deploy/README.md) | 生产部署与排查 |
| [bridge/README.md](bridge/README.md) | 桥接层 |
| [docs/decisions/](docs/decisions/) | 8 条 ADR |
| [CREDITS.md](CREDITS.md) | **素材来源与逐图署名** |
| [docs/ASSETS_LICENSE.md](docs/ASSETS_LICENSE.md) | 素材授权说明 |

## 许可

代码以 **GNU GPL v3** 发布，全文见 [LICENSE](LICENSE)。

**素材不是代码，不适用 GPL。** `data/memory/portraits/` 下的 **129 张图片**
来自社区作者，各自保留原授权，**全部禁止商业使用**：

| 授权 | 数量 | 说明 |
|---|---|---|
| CC BY-NC-SA 4.0 | 33 | 需署名、非商业、衍生同协议 |
| 非营利粉丝向 | 20 | 作者声明可随意使用，商用自评风险 |
| 仅供个人学习使用 | 62 | roseion 皮肤素材，最窄的一档 |
| MIT | 7 | 壁纸 + 三视图立绘 |
| 无 LICENSE | 4 | gmskywalker，授权链不完整 |

逐图来源与署名见 [CREDITS.md](CREDITS.md)，
授权分析见 [docs/ASSETS_LICENSE.md](docs/ASSETS_LICENSE.md)。

> **收进仓库不等于授权变宽。** 商用前必须逐张确认授权；
> 若无法满足署名或非商业条件，请删掉 `data/memory/portraits/`，
> 仅使用 GPL 代码。
