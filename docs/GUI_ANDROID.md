# Android 控制面 APP

跨平台 GUI，用于管理 super-feiyu 的人格设定。
桌面端跑原生窗口，Android 打包成 APK。

## 目录

```
gui/
├── app.py            GUI 源码（Flet）
├── main.py           Flet 打包要求的入口（app.py 的副本）
├── pyproject.toml    项目元数据与打包配置
├── build_apk.ps1     打包脚本（读 keys/ 里的签名配置）
└── build/            产物（已 gitignore）
```

## 桌面运行

```powershell
pip install flet
python gui\app.py
```

## 打包 APK

前置条件：

1. Android SDK（`ANDROID_HOME` 已设置）
2. JDK 21（脚本里硬编码 `C:\Program Files\Android\openjdk\jdk-21.0.8`）
3. Android 许可已接受：`flutter doctor --android-licenses`
4. 签名密钥已生成：`.\make_keystore.ps1`

```powershell
cd gui
.\build_apk.ps1
```

首次约 13 分钟（要编译 Python 运行时），之后增量约 20 秒。

产物：`gui/build/apk/super-feiyu-control.apk`，自动复制到桌面。

## 签名

自签名证书，配置在 `keys/` 目录（**已 gitignore，不进仓库**）。

| 项 | 值 |
|---|---|
| 算法 | RSA 4096 |
| 有效期 | 10000 天 |
| DN | `CN=Sonder-Traveller, OU=super-feiyu, O=Sonder, C=CN` |

密码由 `make_keystore.ps1` 随机生成，存在 `keys/keystore.properties`。

**备份提醒**：keystore 丢了就无法用同一签名更新已安装的 APP，
只能卸载重装。请自行妥善备份，不要放公开仓库或网盘。

## 连接配置

APP 首次打开要填三项：

| 字段 | 说明 |
|---|---|
| 主机 | 跑着 super-feiyu 的机器地址（如 Tailscale 的 `100.64.0.5`） |
| 端口 | 控制面 API 端口，默认 8765 |
| Token | 服务端 `~/super-feiyu/.api_token` 的内容 |

Token 会存到手机本地（`~/.super-feiyu/token`），下次自动填充。

## 服务端要求

`config.toml` 里要开 API：

```toml
[api]
port = 8765
bind = "0.0.0.0"     # 手机要能连，不能只绑 127.0.0.1
auth = true
```

Token 通过环境变量注入，**不要写进配置文件**：

```bash
# systemd unit 里加一行
Environment=FEIYU_API_TOKEN=你的token
```

## 界面

- **宽屏**（平板/横屏）：左侧会话列表 + 右侧编辑器
- **窄屏**（手机竖屏）：单栏，列表与编辑页切换

功能：全局默认人格、每会话独立覆盖、保存/撤销/清除。

## 打包踩过的坑

### 1. Flet 下载 Python 构建清单时 SSL 失败

```
URLError: <urlopen error [SSL: UNEXPECTED_EOF_WHILE_READING]>
```

Python 的 SSL 栈在某些网络环境下读不到 GitHub。
解决办法：用 PowerShell 预先下载清单，再通过环境变量指过去。

```powershell
$env:FLET_PYTHON_BUILD_MANIFEST = "...\gui\manifest.json"
```

### 2. 入口文件必须叫 main.py

Flet 打包找 `main.py`，找不到会报
`main.py not found in the root of Flet app directory`。

### 3. Android 许可未接受

`flutter doctor` 会报 `Some Android licenses not accepted`，
打包直接失败。先跑 `flutter doctor --android-licenses`。

### 4. JAVA_HOME 与 PATH 里的 java 版本不一致

系统 PATH 里是 Java 25，但 Gradle 需要 17/21。
打包脚本里显式设置 `JAVA_HOME` 并前置到 PATH。

### 5. Flet 1.x 的 API 变化

- `ft.padding.only()` → `ft.Padding.only()`
- `ft.run(main)` 取代 `ft.app(target=main)`
- Web 模式需要额外装 `flet-web` 包

## 已知限制

- 只打了 `arm64-v8a`，32 位设备装不上
- Debug 构建未开启（用的是 release + 自签名）
- 没有图标（用 Flet 默认图标）
