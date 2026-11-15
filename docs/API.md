# 控制面 API

只暴露 HTTP 接口，不带界面，由另一个进程（GUI）调用。
用于管理人格（全局默认 + 每会话覆盖）、通道配置与凭据绑定。

完整的端点契约见 [CONTROL_API.md](CONTROL_API.md)（本文只列常用部分）。

## 开启

`config.toml`：

```toml
[api]
port = 8765              # 0 = 不启动（默认）
bind = "127.0.0.1"       # 默认只监听本机
auth = true              # 默认开启鉴权
```

Token 从环境变量 `FEIYU_API_TOKEN` 读，**不写进配置文件**：

```bash
# systemd 用户服务：写进 unit，不要 export 到 shell
# （systemctl restart 不继承 shell 环境，这是踩过的坑）
sed -i '/^\[Service\]/a Environment=FEIYU_API_TOKEN=你的token' \
  ~/.config/systemd/user/super-feiyu.service
systemctl --user daemon-reload
systemctl --user restart super-feiyu
```

**开着鉴权又没设 token 时，服务不会启动**——不静默降级成无鉴权。

## 鉴权

```
Authorization: Bearer <FEIYU_API_TOKEN>
```

- token 比较用常量时间，避免计时攻击
- 缺失或错误一律 401
- `/healthz` 免鉴权（不泄露信息，固定返回 `{"ok":true}`）

## 端点

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
| DELETE | `/v1/persona/session?conv_id=X` | 清除会话覆盖，回落到全局 |
| POST | `/v1/capability/workbuddy/login` | 发起 WorkBuddy 扫码登录 |
| GET | `/v1/capability/workbuddy/login/status?login_id=X` | WorkBuddy 登录状态 |
| POST | `/v1/capability/workbuddy/test` | 测桥接层连通性 |

### 会话列表：点选，不手输

会话人格的 `conv_id` **不需要用户手工输入**。GUI 从 `GET /v1/conversations`
拉会话登记表，渲染成卡片列表（群名 / 昵称 / 渠道图标），点卡片直接进入该会话的人格编辑。

```bash
# 列出机器人见过的所有会话
curl -H "Authorization: Bearer $TOKEN" $B/v1/conversations
# → {"conversations":[
#      {"key":"g:123456","channel":"qq","kind":"group",
#       "title":"测试群","sender_name":"张三","sender_id":"oABC...",
#       "last_active_ms":1730000000000,"handled":12}]}
```

返回的 `key` 就是 `conv_id`（群聊带 `g:` 前缀，私聊是原始 openid），
直接拿它填 `/v1/persona/session?conv_id=`。

### 示例

```bash
TOKEN=your-token
B=http://127.0.0.1:8765

# 探活
curl $B/healthz

# 列出会话（拿 conv_id 的正常途径）
curl -H "Authorization: Bearer $TOKEN" $B/v1/conversations

# 读全局人格
curl -H "Authorization: Bearer $TOKEN" $B/v1/persona/default
# → {"persona": "You are a helpful assistant.\n"}

# 写全局人格
curl -X PUT -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"persona":"你是一条蓝色的鲸鱼"}' \
  $B/v1/persona/default
# → {"ok":true}

# 给某个群设单独人格（conv_id 群聊是 "g:群号"，私聊是 openid）
curl -X PUT -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"persona":"这个群里你是严肃助手"}' \
  "$B/v1/persona/session?conv_id=g:123456789"
# → {"ok":true}

# 清除覆盖，回落到全局
curl -X DELETE -H "Authorization: Bearer $TOKEN" \
  "$B/v1/persona/session?conv_id=g:123456789"
# → {"ok":true}
```

## 人格生效时机

**热加载**：`persona_for()` 每次请求都读盘，改完下一条消息就生效，不用重启。

## 落盘位置

```
data/persona/default.md             全局默认
data/persona/sessions/<hash>.md     会话覆盖
```

会话 id 用 FNV-1a 哈希成 16 位十六进制文件名——`conv_id` 可能含冒号
（`g:123456`）或很长（openid），直接当文件名跨平台不安全。
哈希后无法从文件名反查会话，也不会有路径穿越。

写文件走 `.tmp` + `rename`，避免写一半进程挂了留下破损文件。

## 安全边界

- **默认只监听 `127.0.0.1`**。要对外必须显式改成 `0.0.0.0`，并自己加防火墙。
- 没有 TLS。跨机器调用请走 SSH 隧道或反向代理。
- 没有速率限制。控制面请求量应该很小；被刷说明有别的环节出了问题。
- 只支持扁平 JSON 提取（`{"persona":"..."}`），不解析嵌套结构。

## 状态码

| 码 | 含义 |
|---|---|
| 200 | 成功 |
| 400 | 参数错误（如缺 `conv_id`） |
| 401 | 未授权 |
| 404 | 路径不存在 |
| 500 | 存储读写失败 |
| 502 | 上游（q.qq.com / ilinkai / WorkBuddy 上游）返回异常 |
