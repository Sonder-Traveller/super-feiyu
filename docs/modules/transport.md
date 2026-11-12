# transport 模块设计

> 职责：把平台协议翻译成 `InboundMessage`，把 `OutboundMessage` 翻译回平台协议。
> **不做任何业务判断。**

---

## 1. 定位与边界

### 该做的

- 建立/维护到平台的长连接
- 接收原始事件 → 解析 → 规范化为 `InboundMessage`
- 处理平台特有的心跳、重连、限流语义
- 把 `OutboundMessage` 编码成平台要求的格式并发送
- 重试与错误码映射

### 不该做的

- ❌ "要不要回复"（属于 orchestrator）
- ❌ 拼提示词（属于 persona）
- ❌ 存记忆（属于 store）
- ❌ 决定发几条（属于 ReplyBudget）

---

## 2. 通道对比

| | QQ 官方机器人 | 微信 iLink |
|---|---|---|
| 协议 | WebSocket（网关长连） | HTTP 长轮询 |
| 认证 | AppID + AppSecret → AccessToken | 扫码 → bot_token |
| 消息 id | `msg_id` | `context_token` |
| 回复配额 | 单聊 4 / 群 5（按 msg_id） | 无 |
| 消息时效 | 单聊 60 min / 群 5 min | token 有效期 24 h |
| 群聊触发 | 仅 @ 事件推送 | 不支持群 |
| 富媒体 | `msg_type=7`，需先上传拿 `file_info` | **仅文本** |
| 主动消息 | 已停用 | 限流内可用 |

**结论**：QQ 官方是主力，微信是补充。两者差异大，但都被 `IChannel` 抹平。

---

## 3. QQ 官方机器人通道

### 3.1 连接

```
POST /app/getAppAccessToken  →  access_token（约 7200s 有效）
WS  connect wss://api.sgroup.qq.com/websocket
  → HELLO(op 10) 带 heartbeat_interval
  → IDENTIFY(op 2) 带 token + intents
  → READY(op 0) 带 session_id
  → 循环：心跳(op 1) / 事件(op 0) / 重连(op 7) / 失效(op 9)
```

**Intent 只订阅需要的**：
- `GROUP_AT_MESSAGE_CREATE`（群 @ 消息）
- `C2C_MESSAGE_CREATE`（私聊）

不订阅全部：减少无效事件流量与解析开销。

### 3.2 断线重连

| 情况 | 策略 |
|---|---|
| op 7（服务端要求重连） | 带 `session_id` + `seq` 走 RESUME |
| op 9（session 失效） | 清 session，重新 IDENTIFY |
| 网络断开 | 指数退避重连：1s → 2s → 4s → 最长 60s |
| 心跳超时 | 视为断开 |

**必须实现 RESUME**：否则每次网络抖动都要重新 IDENTIFY，
期间的消息全部丢失。

### 3.3 发送

被动回复必须携带 `msg_id`，且**同一 msg_id 多次回复需递增 `msg_seq`**：

```json
POST /v2/groups/{group_openid}/messages
{
  "content": "文本",
  "msg_type": 0,
  "msg_id": "<原消息 id>",
  "msg_seq": 1
}
```

`attempt_no` 直接映射到 `msg_seq`，重试时必须复用同一 seq（不能递增），
否则平台认为是新的一次回复，会多占配额。

### 3.4 富媒体

发送图片/语音需要两步：

1. 上传：`POST /v2/groups/{id}/files`，`file_type=1(图片)/3(语音)`，得到 `file_info`
2. 发送：`msg_type=7`，`media.file_info`

**注意 `file_info` 有 TTL**，过期需重新上传。
临时文件落地 `data/media/`，发送成功后删除。

---

## 4. 微信 iLink 通道

### 4.1 连接

```
GET  /ilink/bot/get_bot_qrcode?bot_type=3   → qrcode
轮询 /ilink/bot/get_qrcode_status?qrcode=X  → confirmed 后拿 bot_token + baseurl
POST /ilink/bot/getupdates                 → 长轮询，服务端 hold 35s
```

请求头每次都要带：

```
AuthorizationType: ilink_bot_token
X-WECHAT-UIN: <随机 uint32 的 base64，每次重新生成>
Authorization: Bearer <bot_token>
```

### 4.2 必须处理的坑

| 坑 | 后果 | 对策 |
|---|---|---|
| `context_token` 必须用**当前消息**的 | 复用旧 token 消息静默丢失 | 逐消息透传，不缓存 |
| `sendmessage` 字段缺任一 | HTTP 200 但消息不投递 | 严格按完整结构构造 |
| token 24 h 过期 | 断连 | 提前 2 h 预警 + 重扫码 |
| `typing_ticket` 每用户缓存 | 频繁 getconfig | 按 `from_id` 缓存 |

### 4.3 能力声明

```cpp
ChannelCapability{
  .supports_group = false,
  .supports_private = true,
  .supports_image_out = false,
  .supports_voice_out = false,
  .supports_voice_in = false,
  .max_replies_per_message = INT_MAX,
}
```

**声明式降级**：编排层看到 `supports_voice_out=false` 就不调 TTS，
省一次无用的网络请求。

---

## 5. 重试策略

按错误性质区分，**不做无差别重试**：

| 情形 | 重试 | 说明 |
|---|---|---|
| 网络超时 | ✅ 退避 2 次 | 可能已送达，用相同 msg_seq |
| 429 限流 | ✅ 按 `Retry-After` | 无该头则指数退避 |
| 5xx | ✅ 退避 1 次 | |
| 4xx（400/401/403） | ❌ | 参数或权限问题，重试无意义 |
| 被动窗口过期 | ❌ | 回不去了，直接放弃 |

**关键**：重试发送时 `msg_seq` **必须复用**，不能递增——
递增会占用额外配额，甚至触发限流。

---

## 6. 内存考量

| 项 | 目标 |
|---|---|
| WS 缓冲区 | < 1 MB（单条消息有大小上限） |
| 媒体临时文件 | 发送后立即删除 |
| 消息解析 | 流式解析或限制单条最大 1 MB |

**禁止**：把所有历史消息留在内存。只在处理中持有当前消息。

---

## 7. 待定事项

| 事项 | 选项 | 倾向 |
|---|---|---|
| WebSocket 库 | Boost.Beast / libwebsockets / 自写 | **Boost.Beast**（无额外进程，成熟） |
| HTTP 库 | libcurl / cpp-httplib / Boost.Beast | **libcurl**（简单可靠，支持异步） |
| JSON 库 | nlohmann / simdjson / RapidJSON | **nlohmann**（易用，性能足够） |
| TLS | OpenSSL | 跟随所选 HTTP 库 |
| 是否支持频道（guild） | 支持 / 不支持 | 不支持（用户群场景用不到） |

> 依赖引入需评估体积：libcurl + OpenSSL 静态链接约 3–5 MB，可接受。

---

## 8. 测试要点

| 用例 | 方法 |
|---|---|
| 消息解析 | 录制真实平台响应，脱敏后作为 fixture |
| `mentioned_me` 判定 | 构造带/不带 @ 的群消息 |
| 重试不递增 msg_seq | Mock 传输层，断言 seq 相同 |
| 断线 RESUME | Mock WS，验证 op 6 携带 session_id + seq |
| 能力声明与实际一致 | 断言 `supports_*` 与发送实现匹配 |

**通道层测试一律用 Mock，不打真实平台**（避免封号与不稳定）。
