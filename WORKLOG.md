# 工作记录

## 2026-11-13 通宵实现

从设计文档到生产级程序，全部 C++20 模块，一次完成。

---

## 最终状态

| 项 | 结果 |
|---|---|
| 模块数 | 10 个（全部 C++20 modules，无头文件） |
| 代码规模 | 约 6800 行 C++ |
| 测试 | **84 项断言**全绿（单元 60 + 协议 24） |
| 常驻内存 | **18.6 MB** / 4 线程（实测） |
| 二进制 | 2.6 MB（静态链接 curl/sqlite 外的全部逻辑） |
| 构建 | clang 21，干净构建一次通过 |
| 部署 | systemd 用户服务，enabled，已实测启停 |
| 真实 API | 硅基流动免费模型已验证，**零费用** |

对比参考：AstrBot + NapCat 空闲占 **855 MB**。

---

## 模块清单

| 模块 | 职责 | 状态 |
|---|---|---|
| `fish.core` | 领域类型、`Result`、七类接口契约 | ✅ |
| `fish.log` | 结构化单行 JSON 日志 | ✅ |
| `fish.config` | TOML 子集 + `${VAR}` 展开 | ✅ |
| `fish.memory` | 三层记忆、巩固、遗忘、向量 + LIKE 两路检索 | ✅ |
| `fish.capability` | HTTP 客户端、熔断、多 provider 工厂 | ✅ |
| `fish.ws` | libcurl WebSocket + 指数退避自动重连 | ✅ |
| `fish.qq_ws` | QQ 网关协议栈 | ✅ |
| `fish.onebot_ws` | OneBot v11 事件解析 | ✅ |
| `fish.channels` | 四个通道实现 + 富媒体上传 | ✅ |
| `fish.orchestrator` | 流水线、被动策略、预算、去重 | ✅ |

---

## 本轮补齐的缺口

上一轮交付时，`main.cpp` 只实例化 `LocalChannel`，
三个真实通道**从未被使用**，配置文件里的 `channel.*` 无人读取。
本轮全部补齐：

| 缺口 | 修复 |
|---|---|
| QQ 官方收消息 | `fish.qq_ws`：op10 HELLO → op2 IDENTIFY → 心跳 → op6 RESUME |
| OneBot 收消息 | `fish.onebot_ws`：WS 客户端 + 事件解析 |
| 通道装配 | `main` 读 `channel.*`，每通道一个线程 |
| 富媒体发送 | 先 multipart 上传拿 `file_info`，再 `msg_type=7` |
| 断线自愈 | `fish.ws::enable_auto_reconnect()`，指数退避 1s→60s |
| 配置校验 | 缺通道必填项/人格文件/key → 拒绝启动并列全部问题 |
| 优雅退出 | SIGTERM → stopping → shutdown，实测 200ms |
| 生产部署 | systemd 单元 + install.sh + 排查文档 |

---

## 修复的真实缺陷

### 1. 巩固重复写记忆（严重）

连跑两次后 `facts.md` 出现重复行：

```
- [id:f2026-11-13_1] 在一家做芯片的公司上班
- [id:f2026-11-13_1] 在一家做芯片的公司上班   ← 重复
```

根因：id 只在单次调用内自增（`f{today}_{idx}`），跨批次必撞；
且没有内容去重。

**修复**：id 改为内容哈希（FNV-1a 取前 12 位），写入前查重。
连跑两次验证仍是 2 条。

### 2. 纯 @ 消息触发空回复

`{"type":"at"}` + 无文本的消息被当作非空，会走完整流水线回一句废话。

**修复**：过滤 `at`/`reply` 元数据段后再判断是否为空。

### 3. OneBot HTTP 端口与 WS 端口不同

原实现从 `ws://127.0.0.1:3001` 推导 HTTP 地址，但 NapCat 默认
HTTP 是 3000、WS 是 3001。发送会打到 WS 端口上。

**修复**：新增独立 `http_url` 配置项。

---

## 验证覆盖

`./verify.sh` 8 项：

1. 构建（10 模块）
2. 单元测试 60 项（被动红线/预算/去重/三层记忆/巩固/遗忘/索引重建/OneBot 解析）
3. 协议测试 24 项（QQ op 码、@ 清洗、WS 状态机、消息段）
4. 桩模式自检
5. 端到端记忆闭环（巩固 → 落盘 → 召回 → 删索引重建）
6. **WS 收消息 → @ 判定 → HTTP 发送**（mock OneBot 全链路）
7. 缺配置拒绝启动
8. 真实免费 embedding（bge-m3）

关键实测数据：

```
巩固触发: episodes=1 facts=2 traits=0
索引条目数: 3
索引删除后重建成功，重建后条目数: 3   ← 旁路文件确为权威源
```

断线重连实测：

```
peer_closed → reconnect_begin → (1s) → reconnect_ok
```

---

## 关于密钥

- 新的硅基流动 key 只用于**免费模型**：`BAAI/bge-m3`、`FunAudioLLM/SenseVoiceSmall`
- 全程**未调用任何付费模型**，未产生费用
- key 只存 `.env.local`（已 gitignore）与部署目录的 `.env`（权限 600）
- **DeepSeek key 全程未读取任何一次**
- 提交前扫描确认无密钥入库

---

## 环境

- WSL Ubuntu，clang 21.1.8
- 依赖装在 `~/local`（`apt-get download` + `dpkg -x`，无需 root）
- 部署目标 `~/super-feiyu`，systemd 用户服务 `super-feiyu`
- 桥接层凭据读自 `$DSH_HOME/workbuddy-vault`（只读，不落盘）

---

## 已知限制

| 项 | 说明 |
|---|---|
| 长期记忆检索 | 暴力扫描（万级够用），10 万条以上建议换 sqlite-vec |
| 群聊串行化 | 用条带锁（64 槽），碰撞只增加延迟不影响正确性 |
| 富媒体 | QQ 官方需上传，走 curl 命令行；大文件会临时落盘 |
| 微信通道 | 仅单聊文本，媒体需 CDN 加密上传（未实现） |
| 主动消息 | 官方已停用，本项目不做 |

---

## 后续可做

1. QQ 官方频道（guild）支持
2. 记忆的向量索引换成 sqlite-vec
3. 多实例/多鱼隔离
4. 指标暴露 HTTP 端点（当前只有日志）
5. 桥接层支持加密凭据信封

---

## 2026-11-13 补记：原文缓冲加 50% 上限

用户要求「给 buffer 加上限，50% 吧」。这是目前为止**唯一一条**
被明确批准的比例上限（见 ADR-009）。

| 改动 | 文件 |
|---|---|
| `TokenBudget::kRawBufferMaxRatio = 0.50`，`make()` 里硬夹取 | `src/fish.memory.cppm` |
| 新增 `kRawBufferDefaultRatio = 0.40`（未配置时用它，不是用上限） | `src/fish.memory.cppm` |
| 配置 / 文档补一句「硬上限 50%」 | `config.example.toml`、`README.md`、`docs/modules/memory.md` |
| 新增 5 项断言：夹取、不抬高、边界、默认值、不超发 | `tests/test_lifecycle.cpp` |

行为：配置写 0.90 按 0.50 算；写 0.30 按 0.30 算；不写取 0.40。
**默认值故意没动**——加一条上限不该顺手把上下文用量提高 25%，
那是另一件事，需要单独决定。摘要、预留、条数、轮数**均未设上限**。

顺带修掉两个测试文件的历史遗留问题（它们长期没被构建，所以没人发现）：

| 问题 | 原因 | 修复 |
|---|---|---|
| `test_memory.cpp` 编译失败 | 用 `working_rounds`、`MemoryContext::facts`，字段早已不存在 | 改用 `search()` 验证长期记忆可召回 |
| `test_e2e.cpp` 两项断言失败 | 断言 `notes_dir/facts.md`，会话隔离后文件在 `notes/<kind>/<id>/` | 改用 `session_dir(conv)` 拼路径 |

验证：5 个测试二进制全绿（`test_all` / `test_e2e` / `test_protocol` /
`test_lifecycle` / `test_memory`）。

---

## 2026-11-13 补记：文档里的测试数字曾是手抄的

上一节的「测试套件全绿」是真的，但**报出来的数字不对**。核对时发现：

| 项 | 文档声称 | 实测 |
|---|---|---|
| 合计断言 | 306 | **258** |
| 生命周期套件 | 20 | **21** |
| 记忆套件 | 83 | **34** |
| `verify.sh` 实际覆盖 | 5 个套件 | **4 个**（漏 `test_memory`） |

`test_memory` 是这次才修好纳入构建的，`verify.sh` 没跟着加一步，
README 却已经先把它算进总数了——**给没被执行的东西记账**。
和上面 P1「三个二进制没进构建列表」是同一类错误，只是换了个位置。

| 改动 | 文件 |
|---|---|
| 新增「3c. 记忆测试」段，五个套件全部真跑再计数 | `verify.sh` |
| 数字改为实测 258（173 + 24 + 21 + 34 + 6） | `README.md` |
| 复核记录与根因 | `docs/TEST_REPORT.md`（第八节） |

复核：`./verify.sh --no-network` → 通过 9 项，失败 0 项。

> 本文档顶部「最终状态」表是通宵那晚的交付快照（10 模块 / 84 项），
> 保留原样不追改；当前实际是 20 个模块、9216 行、258 项断言。


