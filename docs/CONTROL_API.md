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
      "provider": "openai", "base_url": "http://127.0.0.1:18091/v1",
      "model": "fast-model", "reasoning_effort": "high",
      "api_key_set": true
    },
    "embedding": {"provider": "siliconflow", "base_url": "...", "model": "BAAI/bge-m3",
                  "api_key_set": true},
    "stt":       {"provider": "siliconflow", "base_url": "...", "model": "...",
                  "language": "auto", "api_key_set": true},
    "tts":       {"provider": "edge", "voice": "zh-CN-XiaoyiNeural", "format": "mp3",
                  "group_voice": false},
    "search":    {"provider": "none", "base_url": "...", "api_key_set": false,
                  "max_results": 5},
    "image":     {"provider": "none", "base_url": "", "model": ""}
  },
  "api":    {"port": 8765, "bind": "0.0.0.0", "auth": true},
  "memory": {"model_context_tokens": 1048576, "portraits_dir": "data/memory/portraits",
             "db_path": "data/memory/index.db"},
  "probe":  {"ttl_days": 30, "on_start": true},
  "tool":   {"max_rounds": 3},
  "guard":  {"max_input_bytes": 8000, "max_output_bytes": 4000,
             "rate_limit": 12, "rate_window_s": 60},
  "log":    {"level": "info"}
}
```

类型：`enabled` / `auth` / `on_start` / `group_voice` 是 bool；
`port` / `max_rounds` / `ttl_days` / 各种 `*_bytes` / `*_limit` 是整数；其余是字符串。
密钥字段名一律是 `<原名>_set`（原 `app_secret` → `app_secret_set`）。

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

---

## 7. 运行时状态

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
| 502 | 上游（q.qq.com / ilinkai）返回异常 |
