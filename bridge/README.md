# WorkBuddy 桥接层

> **它不是 super-feiyu 本体的一部分。** 独立进程，可单独启停。
> super-feiyu 只把它当作一个普通的 OpenAI 兼容 provider。

---

## 1. 它解决什么问题

本机已登录 WorkBuddy 桌面端（有积分、有模型）。
想把这些模型接进来用，但**不能**把凭据逻辑和上游协议塞进机器人本体：

- 凭据管理与刷新是独立关注点
- 上游协议是流式 SSE，与 super-feiyu 的「一次请求一次响应」不匹配
- 区域路由（cn / global）的 token 互不通用
- 不应让机器人本体持有任何第三方凭据

所以做成桥接层：**一个进程，对外只说 OpenAI 协议**。

---

## 2. 架构

```
┌──────────────────┐
│  super-feiyu     │  provider = "openai"
│  (capability)    │  base_url = http://127.0.0.1:18081/v1
└────────┬─────────┘
         │  POST /v1/chat/completions（普通 JSON）
         ▼
┌──────────────────┐
│ workbuddy-bridge │  独立进程，本仓库 bridge/
│                  │  ├─ 读凭据（内存中，不落盘）
│                  │  ├─ 转成上游流式请求
│                  │  └─ 聚合 SSE → 一次性响应
└────────┬─────────┘
         │  POST /v2/chat/completions  (stream=true 强制)
         ▼
  copilot.tencent.com  或  www.workbuddy.ai
```

**本体零改动**：super-feiyu 完全不知道自己连的是 WorkBuddy。

---

## 3. 关键实现细节（都是实测出来的）

### 3.1 上游强制流式

非流式请求会被拒绝：

```
HTTP 400 {"code":11101,"msg":"Non-stream chat request is currently not supported"}
```

所以桥接层**必须**发 `stream: true`，然后把 SSE 逐块拼回完整回复。

### 3.2 模型名不是 `deepseek-flash`

上游按网关配置路由，模型 ID 要取自 `/v3/config` 的 `data.models`。
实测可用：

| 模型 ID | 显示名 |
|---|---|
| `auto` | Auto |
| `fast-model` | 快速 |
| `balanced-model` | 均衡 |
| `deep-model` | 极致 |
| `hy3` / `hy4-preview-*` | Hy 系列 |
| `minimax-m2.5` | MiniMax-M2.5 |

传错模型会返回：

```
{"code":11102,"msg":"model [xxx] service info not found"}
```

注意：`fast-model` 实际可能被路由到 `glm-5.3-flash` 等其他后端模型，
响应里的 `model` 字段是**真实后端模型名**。

### 3.3 凭据位置与形态

```
$DSH_HOME/workbuddy-vault/
├── cn/<accountId>.json        ← 优先
└── global/<accountId>.json
```

文件结构：

```json
{
  "version": 1,
  "accountId": "...",
  "auth": {
    "accessToken": "...",      // 可能是明文，也可能是 {"$wbEncrypted":1,...}
    "refreshToken": "...",
    "domain": "www.workbuddy.cn"
  }
}
```

**区域优先于新鲜度**：cn 的 token 对 `copilot.tencent.com` 有效，
拿到 global 网关会 401。

**加密信封**：新版桌面端（Windows 先行）会把 token 加密。遇到这种文件时，
本桥接会跳过它并在多个账号间继续找明文的那份。
如需完整支持加密凭据，应复用 `dsh-connect-workbuddy` 的解析逻辑。

### 3.4 主机白名单

`domain` 字段会被拼进请求。为防止凭据被转发到任意地址，桥接层只允许：

```
copilot.tencent.com
*.tencentbuddy.com
*.workbuddy.ai
*.workbuddy.cn
*.codebuddy.cn
*.codebuddy.ai
```

---

## 4. 用法

```bash
# 启动桥接（凭据自动从 $DSH_HOME 或 ~/.dsh 找）
python3 bridge/workbuddy_bridge.py --port 18081

# 检查状态（不返回 token）
curl http://127.0.0.1:18081/health
# {"status":"ok","version":"0.1.0","credentials":"found"}
```

super-feiyu 侧配置（见 `config.bridge.toml`）：

```toml
[capability.chat]
provider = "openai"
base_url = "http://127.0.0.1:18081/v1"
api_key  = "bridge-local"     # 桥接层不校验，非空即可
model    = "fast-model"
timeout_ms = 90000            # 上游聚合较慢，给足时间
```

**向量仍要单独配**：WorkBuddy / DeepSeek 都没有 embeddings 接口。

---

## 5. 实测结果

### 5.1 桥接层直连

```
$ curl -X POST .../v1/chat/completions \
  -d '{"model":"fast-model","messages":[
        {"role":"system","content":"你叫大肥鱼，说话很短"},
        {"role":"user","content":"你好"}]}'

{"model":"glm-5.3-flash",
 "choices":[{"message":{"content":"你好！我是大肥鱼。有啥事？"}}],
 "usage":{"prompt_tokens":22,"completion_tokens":16}}
```

### 5.2 super-feiyu 全链路

```
$ ./build/super-feiyu --config /tmp/b.toml --live-test
{"module":"capability","event":"chat_ok","provider":"openai","prompt":616,"output":3}
{"module":"pipeline","event":"replied","conversation":"self-test-group","chars":3}

=== SELF TEST ===
sent count: 2 (expected 2)
  -> 在                    ← 群聊 @ 的真实回复
  -> 嗯，你好              ← 私聊的真实回复
SELF TEST PASSED
```

**同时验证了降级**：这次 embedding 指向了不存在的地址，
熔断器打开（`circuit_open`），但对话完全不受影响 —— 这正是
「记忆是增值功能，对话是核心功能」的设计意图。

---

## 6. 限制与注意事项

| 项 | 说明 |
|---|---|
| 仅 Chat | 不含 embedding / TTS / STT |
| 聚合延迟 | 上游流式，桥接聚合后才返回；长回复较慢 |
| 单账号 | 每次请求取第一个可用凭据，不做负载均衡 |
| 凭据刷新 | 依赖桌面端或插件刷新；桥接只读不写 |
| 加密信封 | 暂不支持，会跳过并找明文账号 |
| 并发 | ThreadingHTTPServer，够用但非生产级 |

---

## 7. 为什么不做进本体

三条理由：

1. **凭据隔离**：机器人本体不应该持有任何第三方凭据
2. **协议转换**：SSE → 一次性响应是桥接的职责，不该污染 capability 层
3. **可替换**：不想用 WorkBuddy 时，停掉桥接、把 provider 改回 deepseek 即可，
   本体一行代码不用动

这正是「职责分离」在**进程边界**上的体现。
