# 工作记录

## 2026-11-13 夜间实现

### 完成

**架构落地**：从设计文档到可运行实现，全部 C++20 模块。

| 模块 | 状态 |
|---|---|
| fish.core | ✅ 领域类型、Result、七类接口 |
| fish.log | ✅ 结构化 JSON 日志 |
| fish.config | ✅ TOML 子集 + 环境变量展开 |
| fish.memory | ✅ 三层记忆 + 巩固 + 遗忘 + RRF 检索 |
| fish.capability | ✅ HTTP 客户端 + 4 类能力 × 多 provider |
| fish.transport | ✅ QQ 官方 / QQ 小号 / 微信 / 本地 |
| fish.orchestrator | ✅ 流水线 + 被动策略 + 预算 + 去重 |

**验证**：
- 12 组单元测试全绿
- 端到端记忆闭环通过（含删索引重建）
- 真实 HTTP 链路验证通过（mock server，`cached_tokens` 正确采集）
- 部署到 `~/super-feiyu` 并自检通过

### 关键技术决策

**1. 为什么用模块而不是头文件**
需求方明确要求。clang 21 的模块支持良好，STL 容器可跨模块使用。
踩到的坑记录如下。

**2. 单文件模块 + `module :private;`**
clang **不允许**实现单元 import 自己的模块，也不允许在模块外定义模块类的成员。
所以必须把实现放进接口文件的 private 片段。

**3. 模块不能导出宏**
日志原本是 `LOG_INFO(...)` 宏，在模块内不可见。
改成导出函数 `fish::log::log_line(Level, module, event)`。

**4. import 不转导出**
`import fish.core;` 不会让下游看到 `fish::core`。
下游要用某类型，它必须出现在被 import 模块的 `export namespace` 里。

**5. 消费者需要 include 标准库**
clang 模块的已知限制：用 `std::vector` 等的消费者必须自己 `#include <vector>`。
测试文件与 main.cpp 都显式包含了所需 STL 头。

### 未完成 / 待办

| 项 | 说明 |
|---|---|
| WorkBuddy 桥接层 | 需求方点名要做，**独立进程**，不进本体。见下节设计 |
| QQ 官方 WS 网关 | 当前实现是 HTTP 发送；事件接收需补 WebSocket 线程 |
| 富媒体发送 | QQ 官方需先上传拿 `file_info`，当前只发文本 |
| 真实 API 实测 | 硅基流动 key 余额不足（402），DeepSeek key 按需求未使用 |

---

## WorkBuddy 桥接层设计（未实现）

需求：把本机已登录的 WorkBuddy 模型接入，做成**桥接层而非本体的一部分**。

### 为什么是桥接层

dsh-connect-workbuddy 插件的架构已经是 loopback shim 模式：

```
DSH ──▶ loopback shim（随机端口 + 进程内随机 secret）
          ──▶ copilot.tencent.com/v2/chat/completions
```

真实 token 只在插件进程内存里，不交给下游。
**这正好是 super-feiyu 的 provider 抽象能直接消费的形态。**

### 设计：独立进程 + OpenAI 兼容接口

```
super-feiyu ──▶ http://127.0.0.1:PORT/v1 ──▶ workbuddy-bridge ──▶ 上游
   (capability)                                (独立进程)
```

- 桥接层单独一个进程/二进制，**super-feiyu 本体零改动**
- 对外只暴露 OpenAI 兼容的 `/chat/completions`
- 凭据从 `$DSH_HOME/workbuddy-vault/` 读取，只在桥接进程内存中
- super-feiyu 侧配置即可：
  ```toml
  [capability.chat]
  provider = "openai"        # 走 OpenAI 兼容
  base_url = "http://127.0.0.1:PORT/v1"
  api_key  = "${BRIDGE_TOKEN}"   # 桥接层自己的哨兵 token，非上游凭据
  model    = "workbuddy-xxx"
  ```

### 待确认事项

1. 上游协议细节（`/v2/chat/completions` 的请求/响应格式）需要抓包或读参考实现
2. 凭据刷新机制（vault 里的 token 有时效）
3. 国内版（`copilot.tencent.com`）与国际版（`www.workbuddy.ai`）的区域路由
4. 是否复用插件的 loopback shim 直接转发，还是重新实现

**没有擅自开工的原因**：这涉及凭据管理与上游协议逆向，
且插件自述"TLS SNI 被拦、需要特定设置"——贸然实现容易做出一个不可用的东西。
建议先确认第 1 项的协议格式。

---

## 环境备忘

- WSL Ubuntu，clang 21.1.8，cmake 4.4.4（pip 装的）
- 依赖装在 `~/local`（apt-get download + dpkg -x，无需 root）
- 构建产物 `build/`，部署目标 `~/super-feiyu`
- 硅基流动 key 在 `.env.local`（已 gitignore），**余额不足**
- DeepSeek key 按需求**从未读取、未写入任何文件**
