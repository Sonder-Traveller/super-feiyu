# 控制面 API v2 —— 契约

控制面只暴露 HTTP，不带 WebUI。所有写操作落到磁盘上的**权威文件**：
`config.toml`（非密钥配置）和 `.env.local`（密钥），然后由服务重载。

设计原则：

- **JSON 形状镜像 `config.toml` 的 section 名**（`channel` / `capability` / `api` / `memory`），
  不做二次命名。写操作就是「点号路径 → 配置文件里的 `[section] key`」，没有映射表。
- **密钥只进 `.env.local`，永不进 `config.toml`**，响应里只回「是否已设置」，不回明文。
- **密钥的判定是自动的**：`config.toml` 里值形如 `"${VAR}"` 的键就是密钥。
  它的明文写进 `.env.local` 的 `VAR=...`，`config.toml` 里保持 `"${VAR}"` 不变。
  这条规则是通用的，加新密钥不需要改代码。
- 所有写操作**先备份再原子替换**（写临时文件 → rename），避免半截文件。
- 保留注释：写回时按行匹配只替换目标键，不重排、不丢注释。
- 通道/模型类改动要重启进程才生效，响应里明确告知 `restart_required`。

---

## 通用

```
Authorization: Bearer <FEIYU_API_TOKEN>
Content-Type: application/json
```

错误响应统一 `{"error":"<code>", "detail":"<人读说明>"}`。

### 端点总览

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/healthz` | 探活，免鉴权 |
| GET | `/v1/status` | 运行时状态 |
| GET | `/v1/config` | 读当前生效配置 |
| PUT | `/v1/config` | 部分更新配置 |
| POST | `/v1/restart` | 优雅退出并重启 |
| GET | `/v1/conversations` | 会话登记表（GUI 卡片列表数据源） |
| GET | `/v1/persona/default` | 读全局默认人格 |
| PUT | `/v1/persona/default` | 写全局默认人格 |
| GET | `/v1/persona/list` | 人格清单（全局 + 各会话） |
| GET | `/v1/persona/session?conv_id=X` | 读某会话人格 |
| PUT | `/v1/persona/session?conv_id=X` | 设某会话人格 |
| DELETE | `/v1/persona/session?conv_id=X` | 清除会话覆盖 |
| POST | `/v1/channel/qq/test` | QQ 官方凭据测试 |
| POST | `/v1/channel/wechat/test` | 微信凭据测试 |
| POST | `/v1/channel/qq/qr` | QQ 官方扫码绑定（建任务） |
| GET | `/v1/channel/qq/qr/status?task_id=X` | QQ 扫码状态 |
| POST | `/v1/channel/wechat/qr` | 微信扫码登录 |
| GET | `/v1/channel/wechat/qr/status?qrcode=X` | 微信扫码状态 |
| POST | `/v1/capability/workbuddy/login` | 发起 WorkBuddy 扫码登录 |
| GET | `/v1/capability/workbuddy/login/status?login_id=X` | WorkBuddy 登录状态 |
| POST | `/v1/capability/workbuddy/test` | 测桥接层连通性 |

---

## 1. 探活

`GET /healthz`（免鉴权）

```json
{"ok": true, "version": "0.2.0", "uptime_s": 1234}
```

---

## 2. 配置读写

### GET /v1/config

返回当前生效配置。**形状与 `config.toml` 一致**，密钥字段换成 `*_set: bool`。

```json
{
  "channel": {
    "qq_official": {"enabled": true, "app_id": "1906397285", "app_secret_set": true},
    "qq_onebot": {
      "enabled": false, "ws_url": "ws://127.0.0.1:3001",
      "http_url": "http://127.0.0.1:3000", "self_id": "",
      "access_token_set": false
    },
    "wechat": {"enabled": false, "bot_token_set": true, "bot_base_url": ""}
  },
  "capability": {
    "chat": {
      "provider": "workbuddy", "base_url": "http://127.0.0.1:18091/v1",
      "model": "deep-model", "reasoning_effort": "high",
      "api_key_set": true
    },
    "embedding": {"provider": "siliconflow", "base_url": "...", "model": "BAAI/bge-m3",
                  "api_key_set": true},
    "stt":       {"provider": "siliconflow", "base_url": "...", "model": "...",
                  "language": "auto", "path": "", "need_wav16k": false,
                  "api_key_set": true},
    "tts":       {"provider": "edge", "base_url": "", "model": "", "voice": "zh-CN-XiaoyiNeural",
                  "format": "mp3", "app_id": "", "cluster": "", "group_voice": false},
    "document":  {"provider": "none", "base_url": "https://api.deepseek.com",
                  "api_key_set": true},
    "search":    {"provider": "none", "base_url": "...", "api_key_set": false,
                  "api_engine": "", "max_results": 5},
    "image":     {"provider": "none", "base_url": "", "model": ""}
  },
  "api":    {"port": 8765, "bind": "0.0.0.0", "auth": true},
  "memory": {"model_context_tokens": 1048576, "portraits_dir": "data/memory/portraits",
             "db_path": "data/memory/index.db"},
  "probe":  {"ttl_days": 30, "on_start": true},
  "tool":   {"max_rounds": 0},
  "guard":  {"max_input_bytes": 0, "max_output_bytes": 0,
             "rate_limit": 12, "rate_window_s": 60},
  "log":    {"level": "info"}
}
```

类型：`enabled` / `auth` / `on_start` / `group_voice` / `need_wav16k` 是 bool；
`port` / `max_rounds` / `ttl_days` / 各种 `*_bytes` / `*_limit` 是整数；其余是字符串。
密钥字段名一律是 `<原名>_set`（原 `app_secret` → `app_secret_set`）。

`max_input_bytes` / `max_output_bytes` / `max_rounds` 为 `0` 表示**不限制**
（输入输出不设上限，工具调用轮数由模型自己决定何时停）。

`capability.chat.reasoning_effort` 是**锁定字段**：值固定为 `high`，
GET 一律回显锁定值（不取 `config.toml`，那里可能根本没这个键），
PUT 试图修改会返回 `effort_locked:high`。

### PUT /v1/config

**部分更新**：只处理请求体里出现的字段，没出现的保持原样。形状同 GET，但密钥字段
用原名（`app_secret`，不是 `app_secret_set`）。

```json
{
  "capability": {"chat": {"model": "fast-model", "reasoning_effort": "high",
                          "api_key": "sk-xxx"}},
  "channel": {"qq_official": {"enabled": true, "app_id": "123", "app_secret": "abc"}}
}
```

密钥字段的三种取值：

| 值 | 含义 |
|---|---|
| 非空字符串 | 覆盖写入 `.env.local` |
| `""` | 从 `.env.local` 删除该变量 |
| `"__keep__"` | 不改（前端把已设置但没动过的字段发这个） |

未知的 section / 键返回 400 `unknown_field` + `detail` 指出是哪一条。
**不静默忽略**——静默忽略会让用户以为改成功了。

响应：

```json
{"ok": true, "changed": ["capability.chat.model", "channel.qq_official.enabled"],
 "restart_required": true}
```

`restart_required=true`：有改动需要重启进程（通道、模型、探测这类）。

---

## 3. 渠道连接测试

### POST /v1/channel/qq/test

用请求体里给的凭据（缺省用已保存的）真实换一次 access_token 并拉 `/users/@me`。

```json
// 请求（可空对象 {}）
{"app_id": "1906397285", "app_secret": "xxx"}

// 成功
{"ok": true, "bot_name": "test1", "bot_uin": "4015850486", "share_url": "https://..."}
// 失败（HTTP 仍是 200，用 ok 区分业务结果）
{"ok": false, "error": "invalid_secret", "detail": "token endpoint returned 100007"}
```

### POST /v1/channel/wechat/test

```json
{"ok": true, "bot_id": "..."}
```

---

## 4. QQ 官方机器人扫码绑定

两步：创建任务拿二维码 → 轮询状态。二维码由**后端渲染成 PNG**，
前端只显示图片，不自己画码——各平台字体/渲染差异会导致扫不出来。

### POST /v1/channel/qq/qr

```json
// 请求
{"source": "super-feiyu"}
// 响应
{
  "task_id": "b925bf9f-...",
  "expires_in": 300,
  "qr_png_b64": "iVBORw0KGgo...",
  "qr_content": "https://q.qq.com/qqbot/openclaw/connect.html?task_id=..."
}
```

`qr_content` 也给出来，方便复制或自己截图。

### GET /v1/channel/qq/qr/status?task_id=X

```json
{"status": "pending"}
{"status": "completed", "app_id": "1906397285", "bot_uin": "4015850486", "saved": true}
{"status": "expired"}
{"status": "error", "detail": "..."}
```

**扫码成功即自动写 `.env.local`**（`QQ_APP_ID` / `QQ_APP_SECRET`，权限 600），
同时把 `config.toml` 的 `channel.qq_official.enabled` 设为 true。
AppSecret 明文**只在这一次响应里出现一次**，之后 GET /v1/config 只回 `app_secret_set`。

---

## 5. 微信 iLink 扫码登录

### POST /v1/channel/wechat/qr

```json
{
  "qrcode": "3d0ff03e5eda35b1c052adb7b00916f0",
  "qr_png_b64": "iVBORw0KGgo...",
  "qr_content": "https://liteapp.weixin.qq.com/q/7GiQu1?qrcode=...",
  "expires_in": 300
}
```

### GET /v1/channel/wechat/qr/status?qrcode=X

服务端代为长轮询（微信这个端点会 hold 约 30 秒），前端 2 秒问一次即可。

```json
{"status": "wait"}
{"status": "confirmed", "bot_token_set": true, "bot_base_url": "...", "saved": true}
{"status": "expired"}
{"status": "error", "detail": "..."}
```

确认后自动写 `.env.local`（`WECHAT_BOT_TOKEN`）和 `config.toml`（`channel.wechat.enabled=true`）。

> bot_token 明文不回传，只回 `bot_token_set: true`。

---

## 6. 人格

| 方法 | 路径 |
|---|---|
| GET | `/v1/persona/default` |
| PUT | `/v1/persona/default` |
| GET | `/v1/persona/session?conv_id=X` |
| PUT | `/v1/persona/session?conv_id=X` |
| DELETE | `/v1/persona/session?conv_id=X` |

新增：

### GET /v1/persona/list

```json
{"default_chars": 1187,
 "sessions": [{"conv_id": "g:123456", "chars": 320, "file": "a1b2c3d4e5f60718.md"}]}
```

> 落盘文件名是 conv_id 的 FNV-1a 哈希，**反推不出** conv_id。
> 所以 `sessions/` 旁额外维护 `sessions/index.tsv`（`conv_id<TAB>hash`），
> 写会话人格时追加。`list` 读它来还原 conv_id。

### GET /v1/conversations

会话登记表 —— 机器人见过的**所有会话**（含 bot 自己的卡片）。
GUI 的「人格」页从它拉数据渲染成卡片列表，用户**点卡片**直接进入该会话的人格编辑，
不需要手工输会话 ID。

```json
{
  "conversations": [
    {"key": "qq:1906397285", "channel": "qq_official", "kind": "bot",
     "title": "大肥鱼", "sender_name": "", "sender_id": "4015850486",
     "last_active_ms": 1730000000000, "handled": 0},
    {"key": "g:123456", "channel": "qq", "kind": "group",
     "title": "测试群", "sender_name": "张三", "sender_id": "oABC...",
     "last_active_ms": 1730000000000, "handled": 12},
    {"key": "oXYZ...", "channel": "wechat", "kind": "private",
     "title": "李四", "sender_name": "李四", "sender_id": "oXYZ...",
     "last_active_ms": 1730000000000, "handled": 3}
  ]
}
```

| 字段 | 说明 |
|---|---|
| `key` | 会话唯一标识，即 `session_key()`：群聊带 `g:` 前缀，私聊是原始 openid |
| `kind` | `bot`（机器人自己）/ `group` / `private` |
| `channel` | 普通会话是 `qq` / `wechat`（QQ 官方与 QQ 小号都记 `qq`）；bot 卡片是 `qq_official` / `qq_onebot` / `wechat` |
| `title` | 展示名：群名或对方昵称（会变，仅展示） |
| `sender_name` | 最近一次发言的人 |
| `sender_id` | 对方的稳定标识（openid / QQ 号） |
| `last_active_ms` | 最后活跃时间（Unix 毫秒） |
| `handled` | 该会话累计处理的消息数 |

> bot 卡片（`kind = "bot"`）的 key 带渠道前缀：`qq:<app_id>`（QQ 官方）、
> `ob:<self_id>`（QQ 小号）、`wx:<token 前 8 位>`（微信）。
> 会话人格没有覆盖时，退到该渠道的 bot 级人格。
>
> 普通会话的这个 key 就是 `PUT /v1/persona/session?conv_id=` 的 `conv_id`。
> 登记表为空时返回 `{"conversations": []}`——不报错。

---

## 7. WorkBuddy 桥接层

WorkBuddy 走本机桥接层（独立进程），控制面提供三步：发起扫码 → 查状态 → 测连通。

### POST /v1/capability/workbuddy/login

不用填 Key：拿一个授权链接，用户在浏览器里登录，我们再查结果。
同一个链接也渲染成二维码（`qr_png_base64`），手机扫更顺。

```json
{
  "ok": true,
  "login_id": "wb_1a2b3c4d5e6f7081",
  "auth_url": "https://.../login?state=...",
  "expires_in": 600,
  "qr_png_base64": "iVBORw0KGgo..."
}
```

### GET /v1/capability/workbuddy/login/status?login_id=X

轮询登录结果。用户还没登录时 `done` 为 false，属正常「未完成」。

```json
{"ok": true, "done": false}
{"ok": true, "done": true, "success": true, "nickname": "xxx", "uid": "xxx"}
{"ok": true, "done": true, "success": false, "error": "timeout"}
{"ok": false, "done": true, "error": "session_not_found", "detail": "登录会话不存在或已过期，请重新发起"}
```

> 登录成功后凭据写入 `data/workbuddy.json`（GUI 扫码登录写的就是这个文件），
> 桥接层启动时带 `--cred-file` 指向它才能读到。

### POST /v1/capability/workbuddy/test

问一次桥接层的 `/v1/models`，看它活着没有、模型名还在不在目录里。
**不消耗任何额度。**

```json
{
  "ok": true,
  "models": ["fast-model", "balanced-model", "deep-model", "auto"],
  "logged_in": true,
  "source": "cred-file",
  "region": "cn",
  "nickname": "xxx"
}
```

失败：

```json
{"ok": false, "error": "bridge_unreachable",
 "detail": "桥接层没响应（http://127.0.0.1:18091/v1），先启动 WorkBuddy 桥接"}
```

> 登录状态取自桥的 `/v1/whoami`（回答「桥现在实际拿谁在发请求」），
> 而不是看 `data/workbuddy.json` 文件存不存在。

---

## 8. 重启服务

### POST /v1/restart

扫码绑定 / 改配置后，通道凭据要重启进程才生效。GUI 调这个接口触发优雅退出
（SIGTERM），由 systemd `Restart=always` 拉起新进程。

```json
{"ok": true, "detail": "restarting"}
```

> 服务端先应答再重启（延迟 200 ms），否则客户端收不到响应。

---

## 9. 运行时状态

### GET /v1/status

```json
{
  "version": "0.2.0",
  "uptime_s": 3600,
  "channels": [
    {"name": "qq_official", "connected": true, "received": 42, "errors": 0}
  ],
  "memory": {"rss_mb": 22.6},
  "workers": 4
}
```

---

## 状态码

| 码 | 含义 |
|---|---|
| 200 | 成功 |
| 400 | 请求体不合法 / 未知字段 / 缺必填 |
| 401 | token 缺失或错误 |
| 404 | 未知路径 |
| 413 | 请求体过大（> 1 MB） |
| 500 | 磁盘写入失败等内部错误 |
| 502 | 上游（q.qq.com / ilinkai / WorkBuddy 上游）返回异常 |
