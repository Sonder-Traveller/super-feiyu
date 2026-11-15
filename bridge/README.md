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
│  (capability)    │  base_url = http://127.0.0.1:18091/v1
└────────┬─────────┘
         │  POST /v1/chat/completions（普通 JSON，不鉴权）
         ▼
┌──────────────────┐
│ workbuddy-bridge │  独立进程，本仓库 bridge/
│                  │  ├─ 读凭据（内存中；cred-file 续期时写回）
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

桥接层支持**三个凭据来源**，可同时启用：

| 来源 | 路径 | 优先级 | 刷新 |
|---|---|---|---|
| `--control-url` | super-feiyu 控制面 HTTP 端点 | **最高** | 桥接自己续期（仅内存） |
| `--cred-file` | `data/workbuddy.json`（GUI 扫码登录写入） | 次之 | 桥接自己续期，并写回 |
| vault | `$DSH_HOME/workbuddy-vault/<region>/*.json` | 后备 | 由桌面端/插件负责，桥接只读 |

`--cred-file` 不传时行为与旧版完全一致（只读 vault）。

#### 3.3.0 `--control-url`：跨平台部署的主力路径

**什么时候必须用它**：控制面和桥接层**看到的不是同一个文件系统路径**时。
典型场景是「服务跑在 WSL、桥接层跑在 Windows 宿主」——两边共享同一个
Windows 仓库目录，但路径不同（`\\wsl$\...` vs `C:\...`），是两个不同的文件：

| | 控制面（WSL）写入 | 桥接层（Windows）读 |
|---|---|---|
| 路径 | `~/super-feiyu/data/workbuddy.json` | `C:\Users\Assistant\projects\super-feiyu\data\workbuddy.json` |
| 同一个文件吗 | ❌ | ❌ |

这种布局下 `--cred-file` 永远对不上，**GUI 扫码登录对对话完全无效**。
走 HTTP 就绕开了文件系统视图。

端点与语义：

```
GET {control-url}/v1/capability/workbuddy/credential
Authorization: Bearer <凭据专用令牌>

200 → {"access_token":"...","refresh_token":"...","expires_at":1797062806,
       "domain":"www.workbuddy.cn","nickname":"...","uid":"..."}
404 → {"error":"no_credential"}          # 控制面还没登录过
401 → 令牌不对
```

返回体与 cred-file **完全同构**，所以桥接层复用同一套解析和续期逻辑。

- 令牌：优先 `--control-token`，否则读环境变量 `FEIYU_CREDENTIAL_TOKEN`。
  这应该是控制面的**凭据专用令牌**（`capability.workbuddy.credential_token`，
  默认自动生成到控制面的 `data/control_credential_token.txt`），
  **不要用主令牌**——主令牌能改配置、扫码登录，权限太大。
- 同步时机：启动时拉一次，之后每 5 秒最多拉一次（节流），且只有
  **指纹变了**（换了账号 / 重新登录）才真正重载账号池。
- 回退顺序：`control` → `cred-file` → `vault`。控制面不可达或返回 404 时
  **不会踢掉正在用的凭据**，只是这一轮拿不到新的。
- 续期：`control` 账号的 refresh_token 续期后**只更新内存**，不写回文件
  （控制面那边才是权威）；`cred-file` 账号照旧写回 `.bak` 备份。

#### 3.3.1 `--cred-file` 的文件格式

由 `ControlService::workbuddy_login_status`（`src/fish.control.cppm`）写入：

```json
{
  "version": 1,
  "access_token": "...",
  "refresh_token": "...",
  "expires_at": 1797062806,          // unix 秒
  "domain": "www.workbuddy.cn",      // 决定区域
  "nickname": "大宇",
  "uid": "...",
  "saved_at": 1794643611941          // 毫秒，仅供展示
}
```

- `expires_at` 是 **unix 秒**；`saved_at` 是毫秒。桥接读 `expires_at` 时按量级兜底
  （`> 1e11` 视为毫秒），避免单位写错导致「刚登录就显示过期 500 年」。
- `domain` 决定区域：含 `workbuddy.cn` / `codebuddy.cn` → 国内版；
  含 `workbuddy.ai` / `codebuddy.ai` → 国际版。区域决定刷新端点的主机。

#### 3.3.2 vault 的文件格式

```
$DSH_HOME/workbuddy-vault/
├── cn/<accountId>.json        ← 优先
└── global/<accountId>.json
```

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

#### 3.3.3 token 刷新

`--cred-file` 的凭据在**过期前 2 分钟**自动续期，端点与头部照抄
`dsh-connect-workbuddy` 的 `client.refreshToken`：

```
POST {base}/v2/plugin/auth/token/refresh
X-Refresh-Token: <refresh_token>
X-Auth-Refresh-Source: workbuddy
```

- CN 的 base 是 `https://copilot.tencent.com`；global 跟随凭据自己的域名
  （`www.workbuddy.ai` 或 `www.codebuddy.ai`）。
- 响应 `{"code":0,"data":{"accessToken","refreshToken","expiresIn","domain"}}`。
- `cred-file` 账号成功后写回 `data/workbuddy.json`（更新 `access_token` /
  `refresh_token` / `expires_at` / `saved_at`），**写之前备份 `.bak`**，
  且先写临时文件再原子替换。
- `control` 账号成功后**只更新内存**，不写回任何文件——控制面那边才是权威。
- 刷新失败且 refresh_token 也失效（HTTP 401/403 或业务码 `12153`）时：
  日志明确写「需要重新登录」，请求返回 **401** 与 OpenAI 兼容错误体
  `{"error":{"message":"...","type":"invalid_credentials"}}`，
  **不会**静默拿过期 token 去撞上游。

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

### 3.5 本机回环不鉴权（重要）

桥接层**完全不读取请求头 `Authorization`**（`api_key` 随便填一个非空值即可）。
这是刻意的便利设计：它只服务本机回环，加鉴权只会多一层无意义的配置。

代价是：**任何能连上该端口的人都能免费用本机的 WorkBuddy 额度。**

因此：

- 默认只绑 `127.0.0.1`；
- `--host` 只接受 `127.0.0.1` / `localhost` / `::1`，
  传 `0.0.0.0` 会直接拒绝启动（退出码 2）；
- **不要把该端口暴露到公网、局域网或任何反向代理后面**；
- 如果确实需要跨机访问，先自己加上 token 鉴权，再改绑定地址。

---

## 4. 用法

```bash
# 只读 vault（旧行为）
python3 bridge/workbuddy_bridge.py --port 18091

# 推荐（同一台机器）：接 GUI 扫码登录写入的凭据（自动续期 + 写回）
python3 bridge/workbuddy_bridge.py --port 18091 \
    --cred-file data/workbuddy.json

# 跨平台部署（控制面在 WSL、桥在 Windows 宿主）：从控制面拉凭据
# 优先级最高，不依赖文件系统路径
python3 bridge/workbuddy_bridge.py --port 18091 \
    --control-url http://127.0.0.1:8765 \
    --control-token <凭据专用令牌>

# 检查状态（不返回 token）
curl http://127.0.0.1:18091/health

# 诊断：桥现在实际拿谁在发请求（super-feiyu 的 workbuddy_test 用它）
curl http://127.0.0.1:18091/v1/whoami
# {"region":"cn","domain":"www.workbuddy.cn","nickname":"大宇",
#  "source":"control","expires_in":12345,"logged_in":true}
```

`/v1/whoami` 的 `source` 取值：`control` / `cred-file` / `vault` / `none`。
`expires_in` 为剩余秒数（凭据没带 `expires_at` 时为 `null`）。
**这才是「是否真的登录了」的可靠答案** —— 不要再用「`data/workbuddy.json`
文件存在」来判断登录状态。

跨平台场景的完整步骤（WSL 服务 + Windows 桥）见
`deploy/README.md` 的「WorkBuddy 桥接层」一节。

super-feiyu 侧配置（见 `config.bridge.toml`）：

```toml
[capability.chat]
provider = "openai"
base_url = "http://127.0.0.1:18091/v1"
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
| 多账号 | `control` > `--cred-file` > vault；前者失败/冷却时才轮到下一个 |
| 凭据刷新 | `control` / `cred-file` 由桥接自己续期（前者仅内存，后者写回）；vault 仍由桌面端/插件刷新 |
| 控制面同步 | 每 5 秒最多拉一次；凭据指纹变了才重载账号池 |
| 加密信封 | 暂不支持，会跳过并找明文账号 |
| 并发 | ThreadingHTTPServer，够用但非生产级 |
| **鉴权** | **不校验 Authorization，仅限本机回环，禁止暴露到公网** |

---

## 7. 为什么不做进本体

三条理由：

1. **凭据隔离**：机器人本体不应该持有任何第三方凭据
2. **协议转换**：SSE → 一次性响应是桥接的职责，不该污染 capability 层
3. **可替换**：不想用 WorkBuddy 时，停掉桥接、把 provider 改回 deepseek 即可，
   本体一行代码不用动

这正是「职责分离」在**进程边界**上的体现。
