# 生产部署

## 快速部署

```bash
git clone <repo> && cd super-feiyu
./deploy/install.sh
```

脚本会：检查依赖 → 构建 → 部署到 `~/super-feiyu` → 生成 `config.toml` 与 `.env` 模板 → 注册 systemd 用户服务 → 自检。

## 目录结构

```
~/super-feiyu/
├── build/super-feiyu        可执行文件
├── config.toml              行为配置（需填通道）
├── .env                     密钥（权限 600，勿提交）
├── persona/
│   ├── persona.md           人格正文
│   └── rules.md             行为红线
├── data/
│   ├── memory/notes/        旁路文件（权威源，可手改）
│   └── memory/index.db      SQLite 索引（可删，会自动重建）
└── bridge/                  WorkBuddy 桥接层（可选）
```

## WorkBuddy 桥接层（手动启动）

桥不是 super-feiyu 拉起的，是独立进程、手动启动。

### 同机部署：--cred-file

```bash
# data_dir 的解析口径与 fish.control.cppm 一致：相对 "data" 按 config.toml
# 所在目录解析，部署后即 ~/super-feiyu/data
cd ~/super-feiyu
python3 bridge/workbuddy_bridge.py --port 18091 \
    --cred-file ~/super-feiyu/data/workbuddy.json
```

- 文件暂时不存在不是错误：还没点「登录 WorkBuddy」时桥会提示并在
  下一次请求时自动重载（登录后无需重启桥）。
- 诊断登录状态：`curl http://127.0.0.1:18091/v1/whoami`
  （`source` = `control` / `cred-file` / `vault` / `none`）。

### 跨平台部署：--control-url（控制面在 WSL、桥在 Windows）

**这是「GUI 扫码登录对对话零效果」的根因所在**：两个环境共享同一个
Windows 仓库目录，但**看到的路径不是同一个**——

| | 控制面（WSL）写入 | 桥接层（Windows）读取 |
|---|---|---|
| 路径 | `~/super-feiyu/data/workbuddy.json` | `C:\Users\Assistant\projects\super-feiyu\data\workbuddy.json` |
| 同一个文件吗 | ❌ 不是 | ❌ 不是 |

所以必须让桥**通过 HTTP 向控制面要凭据**，绕开文件系统视图：

```bash
# Windows 宿主上启动桥接层
# 控制面在 WSL 里跑，WSL2 mirrored networking 下 Windows → WSL 的
# 127.0.0.1 是通的（反之亦然）。
$env:FEIYU_CREDENTIAL_TOKEN = (Get-Content \\wsl$\Ubuntu\home\Assistant\super-feiyu\data\control_credential_token.txt).Trim()

python bridge/workbuddy_bridge.py --port 18091 `
    --control-url http://127.0.0.1:8765 `
    --control-token $env:FEIYU_CREDENTIAL_TOKEN
```

也可以用环境变量省掉 `--control-token`：

```cmd
set FEIYU_CREDENTIAL_TOKEN=<令牌>
python bridge\workbuddy_bridge.py --port 18091 --control-url http://127.0.0.1:8765
```

**令牌从哪来**：控制面首次访问凭据端点时自动生成，写在
`data/control_credential_token.txt`（权限 600）。也可以在 `config.toml`
的 `capability.workbuddy.credential_token` 里显式指定（此时它按密钥
处理，值写 `${VAR}` 引用 `.env.local` 里的变量）。

**控制面侧配置**（`config.toml`）：

```toml
[capability.workbuddy]
# 凭据落盘路径。留空 = data/workbuddy.json（控制面自己读）。
# 跨平台时可以让控制面直接写到桥接层能读到的路径：
cred_file = "/mnt/c/Users/Assistant/projects/super-feiyu/data/workbuddy.json"
# 桥接层拉凭据用的独立令牌。留空 = 自动生成到 data/control_credential_token.txt
credential_token = ""
```

**优先级**：`control`（控制面拉的）> `cred-file`（本地文件）> `vault`。
控制面拉不到（不可达 / 还没登录）时自动退回 `--cred-file`，再退回 vault，
日志里会写明当前用的是哪一个。

**安全**：凭据端点返回明文 token，只认凭据专用令牌（对白名单路径生效）。
`api.bind = "0.0.0.0"` 时它对整个局域网可达——请确保令牌足够强，或把
`bind` 改回 `127.0.0.1`。控制面启动时会打一条
`credential_endpoint_exposed` 警告。

详细文档见 `bridge/README.md`。

## 最小可用配置

### 只跑 QQ 小号（推荐给个人）

```toml
[channel.qq_onebot]
enabled = true
ws_url = "ws://127.0.0.1:3001"
http_url = "http://127.0.0.1:3000"
self_id = "你的小号QQ"
```

`.env`：

```
DEEPSEEK_API_KEY=sk-xxx
SILICONFLOW_API_KEY=sk-xxx
```

启动前先跑起 NapCat 或 Lagrange，并配好 OneBot v11 的 WS 与 HTTP。

### 只跑 QQ 官方

```toml
[channel.qq_official]
enabled = true
app_id = "${QQ_APP_ID}"
app_secret = "${QQ_APP_SECRET}"
```

注意：需在 [q.qq.com](https://q.qq.com) 配置 IP 白名单；群聊需把群加入沙箱且群名带「测试」才能添加机器人。

## 服务管理

```bash
systemctl --user start super-feiyu      # 启动
systemctl --user stop super-feiyu       # 停止（SIGTERM，20 秒内优雅退出）
systemctl --user restart super-feiyu    # 重启
systemctl --user status super-feiyu     # 状态
journalctl --user -u super-feiyu -f     # 实时日志
journalctl --user -u super-feiyu --since "10 min ago"
```

部署到 `~/super-feiyu` 后，服务已 `enable`，随登录自启。
`loginctl enable-linger` 已尝试启用，使服务在登出后继续运行。

## 资源限制

systemd 单元里设了：

| 项 | 值 | 含义 |
|---|---|---|
| `MemoryHigh` | 384M | 超过后内核开始回收 |
| `MemoryMax` | 512M | 超过后 OOM 杀进程，systemd 自动重启 |
| `LimitNOFILE` | 4096 | 防 fd 泄漏 |
| `TimeoutStopSec` | 20 | 优雅退出时间 |

**实测常驻内存约 15–30 MB**，远低于限制。若触发 `MemoryMax`，说明有泄漏，需排查。

## 安全加固

单元里已经开启：

- `NoNewPrivileges` — 禁止提权
- `ProtectSystem=strict` — 系统目录只读
- `ProtectHome=read-only` — home 只读，仅 `data/` 与 `/tmp` 可写
- `CapabilityBoundingSet=` — 清空所有 capability
- `RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX` — 只允许网络套接字
- `PrivateTmp` — 独立 /tmp

## 密钥管理

**铁律**：密钥只放 `.env`，不进 `config.toml`、不进代码、不进 git。

`.gitignore` 已覆盖 `.env` 与 `.env.*`。部署脚本生成的 `.env` 权限为 600。

配置里用 `${VAR}` 引用：

```toml
[capability.chat]
api_key = "${DEEPSEEK_API_KEY}"
```

未设置的变量在启动时会报错并列出变量名，不会静默用空值。

## 启动校验

配置有问题时**直接拒绝启动**并列出所有问题，而不是带病运行：

```
{"event":"config_invalid","problem":"channel.qq_onebot.self_id 为空（否则无法判定是否被 @）"}
{"event":"config_invalid","problem":"capability.chat.api_key 为空（检查 DEEPSEEK_API_KEY 环境变量）"}
启动失败：2 个配置问题，见上方日志。
```

检查项：通道必填项、人格文件存在、真实 provider 的 key 非空。

## 排查

### 收不到消息

1. `journalctl --user -u super-feiyu -f` 看有没有 `channel_enabled`
2. 确认 OneBot 端已连上：`curl http://127.0.0.1:3000/get_login_info`
3. **群聊必须 @ 机器人才回**（被动对话红线），私聊直接说就行
4. 确认 `self_id` 与机器人 QQ 号一致，否则判不出被 @

### 回复很慢或卡住

- 看日志里的 `chat_degraded`（模型失败）或 `circuit_open`（熔断）
- WorkBuddy 桥接层聚合流式响应，长回复较慢，可调大 `timeout_ms`

### 内存持续增长

- 检查 `data/memory/index.db` 大小（超过 200MB 该清理了）
- 看是否有 `reconnect_begin` 刷屏（说明上游不稳定）

### 想让服务在重启后自动起

```bash
sudo loginctl enable-linger $USER
systemctl --user enable super-feiyu
```
