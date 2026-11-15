# super-feiyu

C++20 实现的「大肥鱼」机器人内核 —— DeepSeek 拟人化鲸鱼娘的被动对话系统。

**目标环境**：2 核 1G Ubuntu。本机零推理，全部能力走远程 API，
只做协议转发与数据落盘。实测常驻内存 **15–30 MB**。

---

## 状态

| 项 | 状态 |
|---|---|
| 构建 | ✅ clang 22 个 C++20 模块，一次通过 |
| 测试 | ✅ 258 项断言（5 个套件，全绿） |
| 通道 | ✅ QQ 官方 WS / QQ 小号 OneBot WS / 微信 / 本地 |
| 记忆 | ✅ 三层 + 巩固 + 检索 + 索引重建 |
| 文档输入 | ✅ PDF / Office / 图片 / 表格解析（`fish.doc` 独立层） |
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

## 架构：22 个 C++20 模块

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
    │      文档解析（fish.doc）
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
| `fish.doc` | **文档解析层**（PDF/Office/图片/表格） |
| `fish.probe` | 上游能力探测与缓存 |
| `fish.media` | 媒体去重缓存（内容哈希） |
| `fish.image` | 立绘库（manifest 权威 + 目录扫描，**列清单给模型挑**） |
| `fish.meme` | 热梗库（本地优先，**列清单给模型挑**；学到的写回） |
| `fish.session` | 会话注册表 + 每会话互斥 |
| `fish.persona` | 人格分层拼装（稳定前缀 / 动态后缀） |
| `fish.control` | 配置 / 凭据 / 扫码绑定的控制面服务实现 |
| `fish.api` | 控制面 HTTP API（人格管理，带鉴权） |
| `fish.tool` | 工具注册表（记忆 `search_memory` / 图片 `send_image` / 语音 `send_voice` / 热梗 `lookup_meme` / 联网 `web_search` `web_fetch`） |
| `fish.guard` | 输入注入防护 + 输出脱敏 + 限流 |
| `fish.ws` | libcurl WebSocket 客户端 + 自动重连 |
| `fish.qq_ws` | QQ 网关协议栈（op 码/心跳/RESUME） |
| `fish.onebot_ws` | OneBot v11 事件解析 |
| `fish.channels` | 四个通道实现（含富媒体上传） |
| `fish.dispatcher` | 线程池调度，通道线程只投递 |
| `fish.orchestrator` | 流水线、被动策略、预算、去重 |

> 模块必须按依赖顺序编译，见 `build.sh` 里的 `MODULES` 数组；
> `CMakeLists.txt` 的 `fish_modules` 源文件列表与它保持同一顺序。

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

**检索是两路**：向量余弦 + SQL `LIKE` 关键词，向量结果在前、关键词补足。
这是**记忆召回**（模型主动调 `search_memory`），与热梗/图片「列清单让模型挑」
不是一回事——热梗和图片**不做任何匹配**。

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
| 搜索 | `deepseek`（官方端点走服务端工具） | 5 元/1000 次 |
| 文档 | `none` | 按页计费（启用后） |
| 生图 | `none` | — |

各能力的 provider 清单（**只列真托管 DeepSeek 的厂商**）：

| 能力 | 可选 provider |
|---|---|
| 对话 | `deepseek` `workbuddy` |
| 向量 | `ark` `dashscope` `qianfan` `siliconflow` `workbuddy` |
| 语音识别 | `dashscope` `deepseek` `siliconflow` `volcengine` |
| 语音合成 | `dashscope` `deepseek` `edge` `siliconflow` `volcengine` |
| 搜索 | `bocha` `brave` `deepseek` `exa` `generic` `google` `jina` `none` `searxng` `serper` `tavily` `zhipu` |
| 文档 | `deepseek` `none` |
| 生图 | `bridge` `openai` `siliconflow` |

> DeepSeek 没有 embeddings 接口，向量必须另配。见 [ADR-008](docs/decisions/008-two-vendor-keys.md)。
> 清单以 `src/fish.capability.cppm` 的 `*_provider_list()` 为准。

---

## 搜索：两条路，先看清走哪条

搜索有两条**互斥**的实现路径，由「对话端点是不是 DeepSeek 官方」决定。

| | 服务端联网工具 | 本地工具 |
|---|---|---|
| 开关 | `capability.chat.deepseek_web_tools`（默认 true） | `capability.search.provider` + `api_key` |
| 生效条件 | 开关开 **且** 端点是 DeepSeek 官方 | `provider != "none"` **且** key 解析出非空值 |
| 实际执行 | DeepSeek 服务端（边思考边搜、引用原文） | 本机进程（`web_search` / `web_fetch`） |
| 计费 | 按次计费 | 5 元/1000 次（DeepSeek 官方） |

- **对话走 DeepSeek 官方端点** → 走服务端工具，什么都不用配（默认就是这条）。
- **对话走 WorkBuddy 桥接 / 其它 OpenAI 兼容端点** → 走本地工具：
  出厂已配好 `capability.search.provider = "deepseek"`，只差一把 key。
  填 `.env.local` 的 `DEEPSEEK_API_KEY`，或在 GUI「模型」页填搜索的 API Key。

**不静默**：开关在非官方端点时无效（启动日志打 `deepseek_web_tools_inactive`），
但本地搜索照常工作；本地搜索没 key 时启动打 `env_var_missing`，
控制面 `capability.search.key_ready = false`，GUI 的 API Key 输入框会出现。

详见 [docs/modules/capability.md](docs/modules/capability.md#36-两种搜索服务端工具-vs-本地工具)
和 [docs/API.md](docs/API.md#搜索两种方式先看清走哪条)。

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
cp config.example.toml ~/super-feiyu/config.toml   # 先复制模板再改
vim ~/super-feiyu/config.toml    # 启用通道
vim ~/super-feiyu/.env           # 填 key

# 启动
systemctl --user start super-feiyu
journalctl --user -u super-feiyu -f
```

配置模板见 [config.example.toml](config.example.toml)：它是**出厂模板**，
实际运行的配置在 `config.toml`（已 gitignore，不入库）。
密钥一律用 `${VAR}` 引用，不写明文。

**搜索要用的 key**（`.env` / `.env.local` 里加一行即可）：
`DEEPSEEK_API_KEY` —— 对话、搜索、文档解析共用这一把。
没填搜索就不会启用，但**不会静默**：启动日志打 `env_var_missing`，
控制面 `capability.search.key_ready = false`，GUI 也会提示填。
见 [搜索：两条路](#搜索两条路先看清走哪条)。

详见 [deploy/README.md](deploy/README.md)。

---

## GUI 控制台

`gui/app.py`（Flet）连控制面 API，管人格与通道：

```powershell
pip install flet
python gui\app.py
```

「人格」页从 `GET /v1/conversations` 拉会话登记表，渲染成**卡片列表**
（群名 / 昵称 / 渠道图标），**点卡片**直接进该会话的人格编辑——
不需要手工输会话 ID。扫码绑定 QQ / 微信、扫码登录 WorkBuddy 也在这里。

详见 [docs/GUI_ANDROID.md](docs/GUI_ANDROID.md)。

---

## 验证

```bash
./verify.sh              # 全量：构建 + 5 类测试 + 真实 API
./verify.sh --no-network # 跳过真实网络
```

实测输出：

```
✓ 构建（22 个模块）
✓ 单元测试        (173 项)
✓ 协议测试        ( 24 项)
✓ 生命周期测试    ( 21 项)
✓ 记忆测试        ( 34 项)
✓ 端到端测试      (  6 项)
✓ 缺配置拒绝启动
✓ 真实 embedding（bge-m3，免费模型）
```

合计 **258 项断言，全绿**。逐套件统计：`bash tools/count_pass.sh`
（这个脚本直接跑二进制数 `[PASS]`，不数源码里的 `CHECK`）。
数字以实际运行为准，不要手抄——这套文档之前就因为手抄而虚报了 48 项。

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
# 启动命令必须带 --cred-file，否则 GUI「扫码登录 WorkBuddy」
# 写入的凭据（data/workbuddy.json）不会被桥读取，登录等于白发
python3 bridge/workbuddy_bridge.py --port 18091 \
    --cred-file ~/super-feiyu/data/workbuddy.json
```

```toml
[capability.chat]
provider = "workbuddy"
base_url = "http://127.0.0.1:18091/v1"
api_key = "bridge-local"   # 桥接层不校验 key，非空即可
model = "deep-model"
```

诊断登录状态：`curl http://127.0.0.1:18091/v1/whoami`
（`source` = `cred-file` / `vault` / `none`）。

详见 [bridge/README.md](bridge/README.md) 与 [deploy/README.md](deploy/README.md)。

---

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 总览 + 模块清单 |
| [docs/modules/memory.md](docs/modules/memory.md) | **三层记忆**（重点） |
| [docs/modules/capability.md](docs/modules/capability.md) | 多 provider |
| [docs/modules/doc.md](docs/modules/doc.md) | 文档解析层 |
| [docs/modules/transport.md](docs/modules/transport.md) | 通道与协议 |
| [docs/modules/orchestrator.md](docs/modules/orchestrator.md) | 流水线 |
| [docs/CONTROL_API.md](docs/CONTROL_API.md) | 控制面 API 契约 |
| [docs/API.md](docs/API.md) | 控制面 API 速查 |
| [docs/GUI_ANDROID.md](docs/GUI_ANDROID.md) | GUI / Android APP |
| [config.example.toml](config.example.toml) | **配置模板**（复制成 config.toml） |
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
