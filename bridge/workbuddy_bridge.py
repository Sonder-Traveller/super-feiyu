#!/usr/bin/env python3
"""WorkBuddy 桥接层：把本机已登录的 WorkBuddy 变成 OpenAI 兼容接口。

**独立进程，不属于 super-feiyu 本体。**

    super-feiyu ──▶ http://127.0.0.1:PORT/v1 ──▶ 本桥接 ──▶ WorkBuddy 上游

设计要点：
1. 凭据有三个来源（优先级从高到低）：
   - `--control-url`（super-feiyu 控制面 HTTP 拉取）
     —— **最高优先级**。跨平台部署（控制面在 WSL、桥在 Windows 宿主）
       时两边看到的不是同一个路径，`--cred-file` 永远对不上，GUI 扫码登录
       对桥就是零效果；走 HTTP 与文件系统视图无关。
   - `--cred-file`（super-feiyu 控制面「登录 WorkBuddy」写入的 data/workbuddy.json）
     —— 同机部署时的常规做法，支持 refresh_token 自动续期并写回
   - DSH 插件凭据库 `~/.dsh/workbuddy-vault/<region>/*.json`（默认来源）
2. 凭据只在进程内存里，不落盘、不进日志（唯一的写盘是 --cred-file 的刷新写回）
3. 上游强制流式（stream=true），本桥接把 SSE 聚合成一次性响应
4. 多账号轮换：某个账号限流/失败时自动切下一个
5. 区域感知：cn 与 global 的 token 互不通用，各自绑定自己的 domain
6. 主机白名单：防止凭据被转发到任意地址
7. **本机回环不鉴权**：请求头 Authorization 完全不被读取（api_key 随便填）。
   这是刻意的便利设计，所以只应监听 127.0.0.1；**不要暴露到公网**，
   暴露等于把本机 WorkBuddy 的额度借给任何能访问该端口的人。

用法:
    python3 workbuddy_bridge.py [--port 18091] [--cred-file data/workbuddy.json]
                               [--vault PATH] [--region cn|global|auto]
                               [--control-url http://127.0.0.1:8765]
                               [--control-token TOKEN]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

VERSION = "0.3.0"

# 只允许这些主机（防止 token 被转发到任意地址）
ALLOWED_HOST_SUFFIXES = (
    "copilot.tencent.com",
    "tencentbuddy.com",
    "workbuddy.ai",
    "workbuddy.cn",
    "codebuddy.cn",
    "codebuddy.ai",
)

DEFAULT_MODEL = "fast-model"

# 上游要求的最小请求头集（实测：缺 User-Agent 会 400）
UA = "WorkBuddy/5.8.3 WorkBuddy/5.8.3 CLI/5.8.3"

# token 刷新的端点与头部：与 dsh-connect-workbuddy 的 client.refreshToken 同一套。
# CN 走 copilot.tencent.com（聊天网关），global 走 www.workbuddy.ai / www.codebuddy.ai。
# 实测这两个主机上的 /v2/plugin/auth/token/refresh 都返回同一个 JSON 信封
# （{"code":12153,"msg":"...token format error"} 对应伪造的 refresh token）。
CN_CHAT_BASE = "https://copilot.tencent.com"
GLOBAL_BASE = "https://www.workbuddy.ai"
GLOBAL_CB_BASE = "https://www.codebuddy.ai"

# 刷新请求用的 UA / 来源头。上游对「是不是官方客户端」敏感，缺这些头会被拦。
REFRESH_UA = "CLI/2.63.2 CodeBuddy/2.63.2"
REFRESH_SOURCE = "workbuddy"


def region_of(domain: str) -> str:
    """按 domain 判区域。含 .ai → global，其余（含 .cn / copilot.tencent.com）→ cn。"""
    d = (domain or "").strip().lower()
    if d == "workbuddy.ai" or d.endswith(".workbuddy.ai"):
        return "global"
    if d == "codebuddy.ai" or d.endswith(".codebuddy.ai"):
        return "global"
    return "cn"


def refresh_base(region: str, domain: str) -> str:
    """刷新端点所在主机：global 跟随凭据自己的域名，cn 固定走聊天网关。"""
    if region == "global":
        d = (domain or "").strip().lower()
        if d == "codebuddy.ai" or d.endswith(".codebuddy.ai"):
            return GLOBAL_CB_BASE
        return GLOBAL_BASE
    return CN_CHAT_BASE


# ── 凭据池 ──────────────────────────────────────────────
class Account:
    """一个可用账号。

    source 有三种：
      - `control`   （super-feiyu 控制面 HTTP 拉来的，优先级最高）
      - `cred-file` （super-feiyu 扫码登录写入的本地文件）
      - `vault`     （DSH 插件凭据库）
    control / cred-file 账号额外带 refresh_token / expires_at，能自己续期；
    vault 账号沿用旧行为（由桌面端或插件负责刷新，桥接只读）。
    """

    __slots__ = (
        "region", "name", "token", "domain", "source",
        "failures", "cooldown_until",
        "refresh_token", "expires_at", "nickname", "uid",
    )

    def __init__(self, region: str, name: str, token: str, domain: str,
                 source: str = "vault", refresh_token: str = "",
                 expires_at: int = 0, nickname: str = "", uid: str = ""):
        self.region = region
        self.name = name
        self.token = token
        self.domain = domain
        self.source = source
        self.failures = 0
        self.cooldown_until = 0.0
        self.refresh_token = refresh_token
        self.expires_at = expires_at          # unix 秒，0 = 未知
        self.nickname = nickname
        self.uid = uid

    def available(self) -> bool:
        return time.time() >= self.cooldown_until

    def penalize(self, seconds: float) -> None:
        self.failures += 1
        self.cooldown_until = time.time() + seconds

    def reward(self) -> None:
        self.failures = 0
        self.cooldown_until = 0.0

    def expires_in(self) -> int | None:
        """距过期还有多少秒；expires_at 未知时返回 None。"""
        if not self.expires_at:
            return None
        return int(self.expires_at - time.time())

    def needs_refresh(self, lead_s: int = 120) -> bool:
        """过期前 2 分钟就该续期，避免请求刚发出 token 就死。"""
        left = self.expires_in()
        return left is not None and left <= lead_s


# ── cred-file（super-feiyu 扫码登录写入的凭据）────────────
def normalize_epoch_s(value) -> int:
    """把时间戳统一成 unix 秒。

    上游与本文件约定都是秒，但历史上的 vault 里同时存在毫秒字段
    （refreshedAtMs / saved_at），一旦有人照着抄就会差 1000 倍。
    这里按量级兜底，避免「刚登录就显示过期 500 年」这种误判。
    """
    try:
        v = int(value)
    except (TypeError, ValueError):
        return 0
    if v <= 0:
        return 0
    if v > 10**11:          # 毫秒
        v //= 1000
    return v


class CredFile:
    """`data/workbuddy.json` 的读写封装。

    文件由 super-feiyu 的 `ControlService::workbuddy_login_status` 写入：

        {"version":1,"access_token":"...","refresh_token":"...",
         "expires_at":1797062806,"domain":"www.workbuddy.cn",
         "nickname":"...","uid":"...","saved_at":1794643611941}

    `expires_at` 是 **unix 秒**，`saved_at` 是毫秒 —— 单位不同，别混用。
    """

    def __init__(self, path: Path):
        self.path = path
        self._lock = threading.Lock()

    def exists(self) -> bool:
        return self.path.is_file()

    def load(self) -> Account | None:
        """读文件并转成 Account；文件缺失/不合法返回 None（不抛异常）。"""
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except Exception:
            return None
        if not isinstance(data, dict):
            return None
        token = data.get("access_token")
        domain = data.get("domain")
        if not isinstance(token, str) or not token:
            return None
        if not isinstance(domain, str) or not domain:
            return None
        region = region_of(domain)
        refresh_token = data.get("refresh_token")
        nickname = data.get("nickname")
        uid = data.get("uid")
        return Account(
            region=region,
            name=(nickname if isinstance(nickname, str) and nickname else "cred-file"),
            token=token,
            domain=domain,
            source="cred-file",
            refresh_token=refresh_token if isinstance(refresh_token, str) else "",
            expires_at=normalize_epoch_s(data.get("expires_at")),
            nickname=nickname if isinstance(nickname, str) else "",
            uid=uid if isinstance(uid, str) else "",
        )

    def save(self, acc: Account) -> bool:
        """把刷新后的凭据写回，格式与 C++ 侧一致（2 空格缩进、UTF-8 无 BOM）。

        写之前先把原文件复制成 `<path>.bak`；写失败不会破坏原文件
        （先写临时文件再替换）。
        """
        with self._lock:
            try:
                old = json.loads(self.path.read_text(encoding="utf-8"))
                if not isinstance(old, dict):
                    old = {}
            except Exception:
                old = {}
            payload = dict(old)
            payload["version"] = old.get("version", 1)
            payload["access_token"] = acc.token
            if acc.refresh_token:
                payload["refresh_token"] = acc.refresh_token
            payload["expires_at"] = acc.expires_at
            payload["domain"] = acc.domain
            if acc.nickname:
                payload["nickname"] = acc.nickname
            if acc.uid:
                payload["uid"] = acc.uid
            payload["saved_at"] = int(time.time() * 1000)
            try:
                if self.path.is_file():
                    shutil.copy2(self.path, self.path.with_suffix(self.path.suffix + ".bak"))
                tmp = self.path.with_suffix(self.path.suffix + ".tmp")
                tmp.write_text(
                    json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8",
                )
                os.replace(tmp, self.path)
                return True
            except Exception as e:
                sys.stderr.write(f"[bridge] 写回凭据失败: {type(e).__name__}\n")
                return False


class ControlSource:
    """从 super-feiyu 控制面拉 WorkBuddy 凭据。

    存在的理由是**跨平台**：控制面跑在 WSL、桥接层跑在 Windows 宿主时，
    两边看到的不是同一个文件系统视图（`\\\\wsl$\\...` 与 `C:\\...` 是两个
    不同的文件），`--cred-file` 永远对不上 —— GUI 扫码登录写进了 WSL 的
    `~/super-feiyu/data/workbuddy.json`，桥却在 Windows 的
    `C:\\Users\\Assistant\\projects\\super-feiyu\\data\\workbuddy.json` 里找，
    于是「登录成功」对对话完全无效。

    走 HTTP 就绕开了文件系统：控制面把凭据原样吐出来，桥拿到的
    内容和读本地文件完全一样。

    端点：`GET {base}/v1/capability/workbuddy/credential`
      - 200 → 凭据 JSON（access_token / refresh_token / expires_at /
        domain / nickname / uid），与 cred-file 同构
      - 404 → {"error":"no_credential"}，控制面还没登录过
    鉴权：`Authorization: Bearer <token>`，token 是控制面的**凭据专用
    令牌**（不是主 token）—— 主 token 能改配置、扫码登录，权限太大。
    """

    ENDPOINT = "/v1/capability/workbuddy/credential"

    def __init__(self, base_url: str, token: str = "", timeout: int = 8):
        self.base_url = (base_url or "").rstrip("/")
        self.token = token or ""
        self.timeout = timeout
        self._lock = threading.Lock()
        # 最近一次的凭据摘要，用于判断「控制面那边变了没有」
        self._last_fingerprint: str = ""

    @property
    def enabled(self) -> bool:
        return bool(self.base_url)

    def url(self) -> str:
        return self.base_url + self.ENDPOINT

    def load(self) -> Account | None:
        """拉一次凭据并转成 Account。失败返回 None（不抛异常）。

        返回 None 有两种含义：控制面不可达（网络/鉴权），或者控制面
        明确说「还没登录」（404）。调用方据此决定是退回 cred-file
        还是整体没有凭据。
        """
        if not self.enabled:
            return None
        headers = {"Accept": "application/json"}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        req = urllib.request.Request(self.url(), headers=headers, method="GET")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                body = resp.read()
        except urllib.error.HTTPError as e:
            detail = ""
            try:
                detail = e.read().decode("utf-8", errors="replace")[:200]
            except Exception:
                pass
            if e.code == 404:
                # 控制面说还没登录：不是错误，是「用户还没扫码」
                sys.stderr.write(
                    "[bridge] 控制面还没有 WorkBuddy 凭据（未登录）\n")
            else:
                sys.stderr.write(
                    f"[bridge] 拉控制面凭据失败: HTTP {e.code}: {detail}\n")
            return None
        except Exception as e:
            sys.stderr.write(
                f"[bridge] 拉控制面凭据失败（控制面不可达）: "
                f"{type(e).__name__}: {e}\n")
            return None

        try:
            data = json.loads(body.decode("utf-8", errors="replace"))
        except Exception:
            sys.stderr.write("[bridge] 控制面返回的不是 JSON\n")
            return None
        if not isinstance(data, dict):
            sys.stderr.write("[bridge] 控制面返回的格式不对\n")
            return None

        token = data.get("access_token")
        domain = data.get("domain")
        if not isinstance(token, str) or not token:
            sys.stderr.write("[bridge] 控制面返回的凭据缺 access_token\n")
            return None
        if not isinstance(domain, str) or not domain:
            sys.stderr.write("[bridge] 控制面返回的凭据缺 domain\n")
            return None

        refresh_token = data.get("refresh_token")
        nickname = data.get("nickname")
        uid = data.get("uid")
        acc = Account(
            region=region_of(domain),
            name=(nickname if isinstance(nickname, str) and nickname else "control"),
            token=token,
            domain=domain,
            source="control",
            refresh_token=refresh_token if isinstance(refresh_token, str) else "",
            expires_at=normalize_epoch_s(data.get("expires_at")),
            nickname=nickname if isinstance(nickname, str) else "",
            uid=uid if isinstance(uid, str) else "",
        )
        with self._lock:
            self._last_fingerprint = self._fingerprint(acc)
        return acc

    @staticmethod
    def _fingerprint(acc: Account) -> str:
        """凭据指纹：变了才算「控制面那边更新了」。

        只比 token 的哈希前几位：够区分「换了账号 / 重新登录」，
        又不会把 token 明文写进内存外的任何地方。
        """
        h = hashlib.sha256(acc.token.encode("utf-8")).hexdigest()
        return f"{h[:16]}:{acc.expires_at}:{acc.refresh_token[-8:] if acc.refresh_token else ''}"

    def fingerprint(self) -> str:
        with self._lock:
            return self._last_fingerprint


class RefreshError(Exception):
    """token 刷新失败。needs_relogin=True 表示 refresh_token 也废了。"""

    def __init__(self, detail: str, needs_relogin: bool = False):
        super().__init__(detail)
        self.detail = detail
        self.needs_relogin = needs_relogin


def refresh_credential(acc: Account, timeout: int = 20) -> None:
    """用 refresh_token 换新的 access_token，成功后就地更新 acc。

    端点与头部照抄 dsh-connect-workbuddy 的 `client.refreshToken`：
        POST {base}/v2/plugin/auth/token/refresh
        X-Refresh-Token: <refresh_token>
        X-Auth-Refresh-Source: workbuddy
    响应是 {code, msg, data:{accessToken, refreshToken?, expiresIn?, domain?}}。
    """
    if not acc.refresh_token:
        raise RefreshError("没有 refresh_token", needs_relogin=True)

    base = refresh_base(acc.region, acc.domain)
    if not host_allowed(base.replace("https://", "").split("/")[0]):
        raise RefreshError(f"host not allowed: {base}", needs_relogin=False)

    req = urllib.request.Request(
        base + "/v2/plugin/auth/token/refresh",
        data=b"",
        headers={
            "X-Refresh-Token": acc.refresh_token,
            "X-Auth-Refresh-Source": REFRESH_SOURCE,
            "Accept": "application/json, text/plain, */*",
            "X-Requested-With": "XMLHttpRequest",
            "Origin": base,
            "Referer": base + "/",
            "User-Agent": REFRESH_UA,
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=timeout,
                                    context=ssl.create_default_context()) as resp:
            body = resp.read()
    except urllib.error.HTTPError as e:
        # 401/403 就是 refresh_token 本身不被接受 → 必须重新登录
        detail = ""
        try:
            detail = e.read().decode("utf-8", errors="replace")[:200]
        except Exception:
            pass
        raise RefreshError(
            f"HTTP {e.code}: {detail}",
            needs_relogin=e.code in (400, 401, 403),
        )
    except Exception as e:
        # 网络问题不算凭据失效，让调用方稍后重试
        raise RefreshError(f"{type(e).__name__}: {e}", needs_relogin=False)

    try:
        env = json.loads(body.decode("utf-8", errors="replace"))
    except Exception:
        raise RefreshError("刷新响应不是 JSON", needs_relogin=False)
    if not isinstance(env, dict):
        raise RefreshError("刷新响应格式不对", needs_relogin=False)

    code = env.get("code", 0)
    data = env.get("data") if isinstance(env.get("data"), dict) else {}
    token = data.get("accessToken") if isinstance(data.get("accessToken"), str) else ""
    if code not in (0, 200) or not token:
        msg = env.get("msg") if isinstance(env.get("msg"), str) else ""
        # 业务码 12153 是 refresh token 本身失效，属于必须重新登录
        raise RefreshError(
            f"code={code} msg={msg[:160]}",
            needs_relogin=code in (12153, 401, 403),
        )

    acc.token = token
    new_rt = data.get("refreshToken")
    if isinstance(new_rt, str) and new_rt:
        acc.refresh_token = new_rt
    expires_in = data.get("expiresIn")
    if isinstance(expires_in, (int, float)) and expires_in > 0:
        acc.expires_at = int(time.time() + expires_in)
    else:
        expires_at = data.get("expiresAt")
        if expires_at is not None:
            acc.expires_at = normalize_epoch_s(expires_at)
    new_domain = data.get("domain")
    if isinstance(new_domain, str) and new_domain:
        acc.domain = new_domain
        acc.region = region_of(new_domain)
    acc.reward()


class AccountPool:
    """多账号池：优先用健康的，失败自动冷却并切换。

    顺序即优先级：`control`（控制面拉的）> `cred-file`（本地文件）> `vault`。
    用户刚扫码登录的那个账号就该先被用上，无论它是从哪来的。
    """

    def __init__(self, vault: Path | None, prefer_region: str = "auto",
                 cred_file: CredFile | None = None,
                 control: ControlSource | None = None):
        self.vault = vault
        self.prefer_region = prefer_region
        self.cred_file = cred_file
        self.control = control
        self.accounts: list[Account] = []
        self.last_used: Account | None = None
        self._lock = threading.Lock()
        self._refresh_lock = threading.Lock()
        self._cred_mtime: float = 0.0
        # 控制面凭据的上次指纹 + 上次拉取时刻 + 缓存的那一份。
        # 拉取有网络开销，不能每个请求都拉一遍；按间隔节流。
        self._control_fp: str = ""
        self._control_pulled_at: float = 0.0
        self._control_acc: Account | None = None
        self._rr = 0
        self.reload()

    def _cred_file_mtime(self) -> float:
        if self.cred_file is None:
            return 0.0
        try:
            return self.cred_file.path.stat().st_mtime
        except OSError:
            return 0.0

    # 控制面拉取节流间隔（秒）。登录后希望「很快就生效」，
    # 但每个请求都发一次 HTTP 也没必要 —— 5 秒足够快。
    CONTROL_POLL_INTERVAL_S = 5.0

    def maybe_pull_control_credential(self, force: bool = False) -> None:
        """从控制面拉一次凭据；控制面那边换了账号才真正重载。

        桥接常常比「点登录」先启动，没有这一步就得重启进程登录才生效。
        按时间节流 + 比指纹：既不会每个请求都打一次控制面，也不会漏掉
        「用户重新登录换了账号」。

        控制面拉不到（不可达 / 没登录）时**不动**已有账号：控制面掉线
        不应该把正在用的凭据踢掉，退回 cred-file / vault 是 reload 的
        职责，不是这里的。
        """
        if self.control is None or not self.control.enabled:
            return
        now = time.time()
        if not force and (now - self._control_pulled_at) < self.CONTROL_POLL_INTERVAL_S:
            return
        self._control_pulled_at = now

        acc = self.control.load()
        if acc is None:
            return
        fp = self.control.fingerprint()
        if fp and fp == self._control_fp:
            return   # 没变化，不重载
        self._control_fp = fp
        # 缓存住这一份，reload() 直接用它 —— 别在持锁时再发一次 HTTP。
        self._control_acc = acc
        with self._refresh_lock:
            self.reload()
            cur = next((a for a in self.accounts if a.source == "control"), None)
            if cur is not None:
                sys.stderr.write(
                    f"[bridge] 已从控制面同步 WorkBuddy 凭据（账号 {cur.name}）\n")

    def maybe_reload_cred_file(self) -> None:
        """data/workbuddy.json 被改过（用户刚扫码登录）就重新加载。

        桥接常常比「点登录」先启动，没有这一步就得重启进程登录才生效。
        只看 mtime，不轮询文件内容，开销可以忽略。
        """
        if self.cred_file is None:
            return
        mtime = self._cred_file_mtime()
        if mtime == self._cred_mtime:
            return
        with self._refresh_lock:
            if self._cred_file_mtime() == self._cred_mtime:
                return
            self.reload()
            acc = next((a for a in self.accounts if a.source == "cred-file"), None)
            if acc is not None:
                sys.stderr.write(
                    f"[bridge] 检测到 cred-file 更新，已重新加载账号 {acc.name}\n")

    def reload(self) -> None:
        accounts: list[Account] = []

        # 1) control 最优先：从控制面拉的凭据（跨平台部署的主力路径）
        #
        # 用缓存的那一份，不在这里发 HTTP —— reload() 会在持 _refresh_lock
        # 的情况下被调用，不能在里面做网络 I/O（会把所有请求堵住）。
        if self.control is not None and self.control.enabled:
            if self._control_acc is None:
                # 首次：还没拉过（启动路径），这时拉一次
                self._control_acc = self.control.load()
                if self._control_acc is not None:
                    self._control_fp = self.control.fingerprint()
                self._control_pulled_at = time.time()
            if self._control_acc is not None:
                accounts.append(self._control_acc)

        # 2) cred-file 次之：刚登录的账号就该先被用上
        if self.cred_file is not None:
            acc = self.cred_file.load()
            if acc is not None:
                accounts.append(acc)
            self._cred_mtime = self._cred_file_mtime()

        # 3) vault 里的账号跟在后面
        if self.vault is not None and self.vault.is_dir():
            for region in ("cn", "global"):
                d = self.vault / region
                if not d.is_dir():
                    continue
                for f in sorted(d.glob("*.json")):
                    try:
                        data = json.loads(f.read_text(encoding="utf-8"))
                    except Exception:
                        continue
                    auth = data.get("auth") or {}
                    token = auth.get("accessToken")
                    domain = auth.get("domain")
                    if not isinstance(token, str) or not token or token.startswith("{"):
                        continue          # 加密信封暂不支持
                    if not isinstance(domain, str) or not domain:
                        continue
                    name = (data.get("account") or {}).get("nickname") or f.stem[:8]
                    accounts.append(Account(region, name, token, domain, source="vault"))

        # 区域过滤：指定 cn / global 时只保留该区域的账号。
        # 早期版本只做「排序偏好」，轮询仍会轮到 global，导致
        # 11128 request illegal（设备指纹拦截）污染整个请求。
        if self.prefer_region in ("cn", "global"):
            only = [a for a in accounts if a.region == self.prefer_region]
            if only:
                accounts = only

        with self._lock:
            self.accounts = accounts

    # 优先账号的 source 顺序（从高到低）。
    #
    # control 在 cred-file 前面：控制面是「权威的那个」（GUI 登录写进去的
    # 就是它），cred-file 只是同机部署时的本地副本。两者一致时无所谓，
    # 不一致时以控制面为准。
    PRIORITY_SOURCES = ("control", "cred-file")

    def pick(self) -> Account | None:
        """取一个账号：优先用刚登录的那个，其余按轮询。

        用户刚在 GUI 扫码登录，期望「登录完就能用」；如果还按轮询让 vault 里的
        旧账号先上，登录看起来就毫无效果 —— 这正是本次要修的 bug。
        """
        with self._lock:
            if not self.accounts:
                return None
            # 1) 优先：control / cred-file 账号（健康时）
            for src in self.PRIORITY_SOURCES:
                for acc in self.accounts:
                    if acc.source == src and acc.available():
                        return acc
            # 2) 其余账号轮询
            n = len(self.accounts)
            for i in range(n):
                idx = (self._rr + i) % n
                acc = self.accounts[idx]
                if acc.available():
                    self._rr = (idx + 1) % n
                    return acc
            # 全部冷却中：返回冷却最早结束的
            return min(self.accounts, key=lambda a: a.cooldown_until)

    def ensure_fresh(self, acc: Account, timeout: int = 20) -> None:
        """control / cred-file 账号快过期或已过期时先续期，再拿去发请求。

        续期成功后：
          - cred-file 账号写回 `data/workbuddy.json`（本地副本保持新鲜）
          - control 账号**不写回**（控制面那边才是权威，让它自己管；
            桥只在自己内存里换新 token。下次拉取会拿到控制面那份）
        失败抛 RefreshError，由 HTTP 层翻成 401 + 「需要重新登录」。
        vault 账号不走这里（沿用旧行为：由桌面端/插件负责刷新）。
        """
        if acc.source not in ("control", "cred-file"):
            return
        if not acc.needs_refresh():
            return
        with self._refresh_lock:
            # 双重检查：并发请求可能已经有一个刷新好了
            if not acc.needs_refresh():
                return
            try:
                refresh_credential(acc, timeout=timeout)
            except RefreshError as e:
                if e.needs_relogin:
                    raise
                # 网络抖动之类：token 还没真过期就先用旧的顶着
                if (acc.expires_in() or 0) > 0:
                    sys.stderr.write(
                        f"[bridge] 续期失败（暂用旧 token）: {e.detail}\n")
                    return
                raise
            if acc.source == "cred-file" and self.cred_file is not None:
                ok = self.cred_file.save(acc)
                sys.stderr.write(
                    "[bridge] 已续期 cred-file 凭据"
                    + ("并写回\n" if ok else "（写回失败，仅内存生效）\n"))
            else:
                sys.stderr.write("[bridge] 已续期控制面凭据（仅内存生效）\n")

    def peek(self) -> Account | None:
        """看「接下来会优先用谁」，但**不推进轮询游标**。

        `pick()` 有副作用（`_rr` 前进），而 `/v1/whoami` 是只读诊断端点，
        调它不该改变负载均衡的节奏。
        """
        with self._lock:
            if not self.accounts:
                return None
            for src in self.PRIORITY_SOURCES:
                for acc in self.accounts:
                    if acc.source == src and acc.available():
                        return acc
            return self.accounts[0]

    def whoami(self) -> dict:
        """当前实际会用的账号（不含 token），给 /v1/whoami 与 GUI 诊断用。"""
        acc = self.last_used or self.peek()
        if acc is None:
            return {
                "region": "",
                "domain": "",
                "nickname": "",
                "source": "none",
                "expires_in": None,
                "logged_in": False,
            }
        left = acc.expires_in()
        return {
            "region": acc.region,
            "domain": acc.domain,
            "nickname": acc.nickname or acc.name,
            "source": acc.source,
            "expires_in": left,
            "logged_in": left is None or left > 0,
        }

    def stats(self) -> list[dict]:
        with self._lock:
            return [
                {
                    "region": a.region,
                    "name": a.name,
                    "domain": a.domain,
                    "source": a.source,
                    "failures": a.failures,
                    "available": a.available(),
                    "cooldown_s": max(0, int(a.cooldown_until - time.time())),
                    "expires_in": a.expires_in(),
                }
                for a in self.accounts
            ]


def host_allowed(host: str) -> bool:
    return any(host == s or host.endswith("." + s) for s in ALLOWED_HOST_SUFFIXES)


# ── 上游调用 ────────────────────────────────────────────
def call_upstream(acc: Account, payload: dict, timeout: int) -> dict:
    """调用上游流式接口，聚合 SSE 成一次性响应。"""
    if not host_allowed(acc.domain):
        raise RuntimeError(f"host not allowed: {acc.domain}")

    body = dict(payload)
    body["stream"] = True

    req = urllib.request.Request(
        f"https://{acc.domain}/v2/chat/completions",
        data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {acc.token}",
            "Content-Type": "application/json",
            "Accept": "text/event-stream",
            "User-Agent": UA,
        },
        method="POST",
    )

    text_parts: list[str] = []
    usage = None
    model_name = body.get("model", DEFAULT_MODEL)
    finish = "stop"
    # 工具调用必须按 index 累积：流式里 arguments 是分片到达的
    # （第一片给 name + 部分 arguments，后续片只给 arguments 片段）。
    # 早期只聚合 delta.content，tool_calls 整段被丢弃，
    # 导致调用方只看到 finish_reason=tool_calls 却拿不到任何调用，
    # 于是只能退回「让模型输出 [PHOTO] 文本标记」这种笨办法。
    tool_calls: dict[int, dict] = {}

    with urllib.request.urlopen(req, timeout=timeout,
                                context=ssl.create_default_context()) as resp:
        for raw in resp:
            line = raw.decode("utf-8", errors="replace").strip()
            if not line or not line.startswith("data:"):
                continue
            data = line[5:].strip()
            if data == "[DONE]":
                break
            try:
                chunk = json.loads(data)
            except Exception:
                continue
            if chunk.get("model"):
                model_name = chunk["model"]
            if chunk.get("usage"):
                usage = chunk["usage"]
            for choice in chunk.get("choices") or []:
                if choice.get("finish_reason"):
                    finish = choice["finish_reason"]
                delta = choice.get("delta") or {}

                piece = delta.get("content")
                if piece:
                    text_parts.append(piece)

                for tc in delta.get("tool_calls") or []:
                    idx = tc.get("index", 0)
                    slot = tool_calls.setdefault(
                        idx,
                        {"id": "", "type": "function",
                         "function": {"name": "", "arguments": ""}},
                    )
                    if tc.get("id"):
                        slot["id"] = tc["id"]
                    fn = tc.get("function") or {}
                    if fn.get("name"):
                        slot["function"]["name"] += fn["name"]
                    if fn.get("arguments"):
                        slot["function"]["arguments"] += fn["arguments"]

    message: dict = {"role": "assistant", "content": "".join(text_parts)}
    if tool_calls:
        # 补 id：上游偶尔不给，调用方需要它来配对工具结果
        for idx, slot in tool_calls.items():
            if not slot["id"]:
                slot["id"] = f"call_{idx}"
        message["tool_calls"] = [tool_calls[k] for k in sorted(tool_calls)]

    return {
        "id": "wb-bridge",
        "object": "chat.completion",
        "model": model_name,
        "choices": [
            {
                "index": 0,
                "message": message,
                "finish_reason": finish,
            }
        ],
        "usage": usage or {},
    }


# ── HTTP 服务 ───────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    server_version = f"WorkBuddyBridge/{VERSION}"
    pool: AccountPool = None  # type: ignore
    timeout_s = 120
    debug = False

    def log_message(self, fmt, *args):
        if self.debug:
            sys.stderr.write("[bridge] " + (fmt % args) + "\n")

    def _json(self, code: int, payload: dict):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.rstrip("/")
        if path in ("/health", "/v1/health"):
            stats = self.pool.stats() if self.pool else []
            healthy = [s for s in stats if s["available"]]
            self._json(200, {
                "status": "ok",
                "version": VERSION,
                "accounts": len(stats),
                "healthy": len(healthy),
                "detail": stats,       # 只有账号名/区域，无 token
            })
            return
        if path in ("/v1/whoami", "/whoami"):
            # 给 super-feiyu 的 workbuddy_test 用：回答「桥现在实际拿谁在发请求」，
            # 而不是像以前那样「data/workbuddy.json 存在就算登录」。
            if self.pool:
                self.pool.maybe_pull_control_credential()
                self.pool.maybe_reload_cred_file()
            self._json(200, self.pool.whoami() if self.pool else {
                "region": "", "domain": "", "nickname": "",
                "source": "none", "expires_in": None, "logged_in": False,
            })
            return
        if path in ("/v1/models", "/models"):
            self._json(200, {
                "object": "list",
                "data": [
                    {"id": "fast-model", "object": "model", "owned_by": "workbuddy"},
                    {"id": "balanced-model", "object": "model", "owned_by": "workbuddy"},
                    {"id": "deep-model", "object": "model", "owned_by": "workbuddy"},
                    {"id": "auto", "object": "model", "owned_by": "workbuddy"},
                ],
            })
            return
        self._json(404, {"error": {"message": "not found"}})

    def do_POST(self):
        if not self.path.rstrip("/").endswith("/chat/completions"):
            self._json(404, {"error": {"message": "not found"}})
            return

        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            req = json.loads(raw)
        except Exception:
            self._json(400, {"error": {"message": "invalid json"}})
            return

        payload = {
            "model": req.get("model") or DEFAULT_MODEL,
            "messages": req.get("messages") or [],
        }
        # 透传采样与推理相关字段。
        # reasoning_effort 曾经被丢在这里，导致上游永远用默认档位，
        # 调用方设 high 也毫无效果。
        for key in (
            "temperature",
            "top_p",
            "max_tokens",
            "reasoning_effort",
            "thinking",
            "tool_choice",
            "stop",
        ):
            if key in req:
                payload[key] = req[key]
        # tools 单独处理：上游要求有 tools 时必须显式给 tool_choice
        if req.get("tools"):
            payload["tools"] = req["tools"]
            payload.setdefault("tool_choice", "auto")

        # 多账号重试：失败就换下一个
        #
        # 先问控制面要一次凭据（跨平台部署时这是主力路径），
        # 再看本地 cred-file 有没有变。两个都做完才发请求，
        # 保证「GUI 里刚扫码登录」这一轮就能用上。
        self.pool.maybe_pull_control_credential()
        self.pool.maybe_reload_cred_file()
        tried: set[str] = set()
        last_err = "no account"
        for _ in range(max(1, len(self.pool.accounts))):
            acc = self.pool.pick()
            if acc is None:
                break
            key = f"{acc.region}/{acc.name}"
            if key in tried and len(tried) >= len(self.pool.accounts):
                break
            tried.add(key)
            try:
                # cred-file 账号过期前先续期。续期失败且 refresh_token 已废 →
                # 直接 401 要求重新登录，绝不静默拿过期 token 去撞上游。
                self.pool.ensure_fresh(acc)
                result = call_upstream(acc, payload, self.timeout_s)
                acc.reward()
                self.pool.last_used = acc
                if self.debug:
                    sys.stderr.write(f"[bridge] {key} ok\n")
                self._json(200, result)
                return
            except RefreshError as e:
                last_err = f"{key} refresh failed: {e.detail}"
                sys.stderr.write(
                    f"[bridge] {key} 续期失败（{e.detail}），需要重新登录\n")
                self._json(401, {
                    "error": {
                        "message": "WorkBuddy 登录已失效，需要重新登录"
                                   "（GUI 里点「登录 WorkBuddy」重新扫码）",
                        "type": "invalid_credentials",
                        "detail": e.detail,
                    }
                })
                return
            except urllib.error.HTTPError as e:
                detail = ""
                try:
                    detail = e.read().decode("utf-8", errors="replace")[:200]
                except Exception:
                    pass
                last_err = f"{key} HTTP {e.code}: {detail}"
                if e.code in (401, 403):
                    # 上游说 token 不认。control / cred-file 账号还有 refresh_token
                    # 就再试一次，换完 token 仍 401 才算真的需要重新登录。
                    if acc.source in ("control", "cred-file") and acc.refresh_token:
                        try:
                            with self.pool._refresh_lock:
                                refresh_credential(acc, timeout=20)
                            if acc.source == "cred-file" and self.pool.cred_file is not None:
                                self.pool.cred_file.save(acc)
                            sys.stderr.write(f"[bridge] {key} 401 后已续期，重试\n")
                            result = call_upstream(acc, payload, self.timeout_s)
                            acc.reward()
                            self.pool.last_used = acc
                            self._json(200, result)
                            return
                        except RefreshError as re_:
                            last_err = f"{key} 401 且续期失败: {re_.detail}"
                            sys.stderr.write(
                                f"[bridge] {key} token 失效且续期失败，需要重新登录\n")
                            self._json(401, {
                                "error": {
                                    "message": "WorkBuddy 登录已失效，需要重新登录"
                                               "（GUI 里点「登录 WorkBuddy」重新扫码）",
                                    "type": "invalid_credentials",
                                    "detail": re_.detail,
                                }
                            })
                            return
                        except Exception as re2:
                            last_err = f"{key} 401 重试仍失败: {re2}"
                    else:
                        sys.stderr.write(f"[bridge] {key} 401（vault 账号，需插件侧刷新）\n")
                # 限流/服务端错误 → 冷却该账号；4xx 内容错误 → 不冷却但换号
                acc.penalize(60 if e.code in (429, 500, 502, 503, 504) else 15)
            except Exception as e:
                last_err = f"{key} {type(e).__name__}: {e}"
                acc.penalize(30)

        self._json(502, {"error": {"message": last_err}})


def main():
    ap = argparse.ArgumentParser(description="WorkBuddy → OpenAI 兼容桥接层")
    ap.add_argument("--port", type=int, default=18091)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--vault", default=None)
    ap.add_argument(
        "--cred-file",
        default=None,
        help="super-feiyu 扫码登录写入的 data/workbuddy.json；给了就优先用它，"
             "并支持 refresh_token 自动续期（续期后写回原文件）。默认空 = 不启用。",
    )
    ap.add_argument("--region", default="auto", choices=["auto", "cn", "global"])
    ap.add_argument(
        "--control-url",
        default=None,
        help="super-feiyu 控制面地址（如 http://127.0.0.1:8765）。"
             "给了就从控制面拉 WorkBuddy 凭据，优先级最高。跨平台部署"
             "（控制面在 WSL、桥在 Windows）必须用它——两边看不到同一个文件。",
    )
    ap.add_argument(
        "--control-token",
        default=None,
        help="访问控制面凭据端点的令牌。默认读环境变量 FEIYU_CREDENTIAL_TOKEN；"
             "都没有就不带 Authorization（控制面若要求鉴权会返回 401）。",
    )
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()

    # ── 本机回环安全约束 ──
    # 桥接层不校验 Authorization（api_key 随便填），这是刻意的便利设计。
    # 代价是：任何能连上该端口的人都能免费用本机的 WorkBuddy 额度。
    # 所以只允许绑回环地址；要改成 0.0.0.0 必须先自己加上鉴权。
    LOOPBACK = {"127.0.0.1", "localhost", "::1"}
    if args.host not in LOOPBACK:
        print(f"[bridge] 拒绝监听 {args.host}：本机回环不鉴权，暴露到公网等于"
              "把 WorkBuddy 额度开放给任何人。", file=sys.stderr)
        print("[bridge] 只允许 --host 127.0.0.1 / localhost / ::1", file=sys.stderr)
        return 2

    if args.vault:
        vault = Path(args.vault)
    else:
        dsh = os.environ.get("DSH_HOME")
        cands = [
            Path(dsh) / "workbuddy-vault" if dsh else None,
            Path.home() / ".dsh" / "workbuddy-vault",
            Path(os.environ.get("USERPROFILE", "")) / ".dsh" / "workbuddy-vault",
        ]
        vault = next((c for c in cands if c and c.is_dir()), None)  # type: ignore

    cred_file = CredFile(Path(args.cred_file)) if args.cred_file else None
    if cred_file is not None and not cred_file.exists():
        # 不是错误：用户可能还没点「登录 WorkBuddy」。启动时提示一下，登录后
        # 桥接会在下一次请求时重新加载（见 Handler.do_POST 前的 reload 逻辑）。
        print(f"[bridge] --cred-file 指向的文件暂时不存在: {cred_file.path}",
              file=sys.stderr)
        print("[bridge] 在 GUI 里点「登录 WorkBuddy」后会生成它", file=sys.stderr)

    # 控制面来源：跨平台部署的主力路径
    control = None
    if args.control_url:
        ctrl_token = args.control_token or os.environ.get("FEIYU_CREDENTIAL_TOKEN", "")
        control = ControlSource(args.control_url, ctrl_token)
        if not ctrl_token:
            print("[bridge] 注意：没给 --control-token（也没设 FEIYU_CREDENTIAL_TOKEN），"
                  "拉控制面凭据会不带 Authorization", file=sys.stderr)
            print("[bridge] 控制面若开了鉴权会返回 401；令牌在控制面侧的 "
                  "data/control_credential_token.txt", file=sys.stderr)

    # 一个来源都没有就是真的没得用
    have_vault = vault is not None and vault.is_dir()
    have_cred = cred_file is not None and cred_file.exists()
    if not have_vault and not have_cred and control is None:
        print(f"[bridge] 凭据目录不存在: {vault}", file=sys.stderr)
        print("[bridge] 用 --vault 指定，或用 --cred-file 指定登录凭据文件，"
              "或用 --control-url 从控制面拉", file=sys.stderr)
        return 1

    Handler.pool = AccountPool(vault, args.region, cred_file, control)
    Handler.timeout_s = args.timeout
    Handler.debug = args.debug

    stats = Handler.pool.stats()
    if not stats:
        print("[bridge] 未找到可用凭据，请求将返回 503", file=sys.stderr)
    else:
        # 日志只说「几个账号、来自哪里」，绝不打印 token
        n_control = sum(1 for s in stats if s["source"] == "control")
        n_cred = sum(1 for s in stats if s["source"] == "cred-file")
        n_vault = len(stats) - n_control - n_cred
        origin = []
        if n_control:
            origin.append(f"control({control.url() if control else ''})")
        if n_cred:
            origin.append(f"cred-file({cred_file.path})")
        if n_vault:
            origin.append(f"vault({vault})")
        print(f"[bridge] 载入 {len(stats)} 个账号（优先级: {args.region}；来源: "
              + " + ".join(origin) + "）:", flush=True)
        for s in stats:
            left = s.get("expires_in")
            exp = "unknown" if left is None else f"{left}s"
            print(f"[bridge]   {s['region']:6s} {s['name']:20s} {s['domain']:22s}"
                  f" [{s['source']}] expires_in={exp}", flush=True)
    if control is not None:
        print(f"[bridge] 凭据来源: 控制面 {control.url()}（优先级最高，"
              f"每 {AccountPool.CONTROL_POLL_INTERVAL_S:.0f}s 同步一次）", flush=True)
    if cred_file is not None:
        print(f"[bridge] cred-file 刷新后会写回（备份 .bak）: {cred_file.path}", flush=True)

    try:
        server = ThreadingHTTPServer((args.host, args.port), Handler)
    except OSError as e:
        print(f"[bridge] 无法监听 {args.host}:{args.port}: {e}", file=sys.stderr)
        print("[bridge] 换个端口试试：--port 18092", file=sys.stderr)
        return 1

    print(f"[bridge] listening on http://{args.host}:{args.port}/v1", flush=True)
    print("[bridge] 注意：本机回环不校验 Authorization（api_key 随便填），"
          "不要暴露到公网", flush=True)
    print(f"[bridge] 诊断端点: http://{args.host}:{args.port}/v1/whoami", flush=True)
    print("[bridge] super-feiyu 配置:", flush=True)
    print("[bridge]   [capability.chat]", flush=True)
    print('[bridge]   provider = "openai"', flush=True)
    print(f'[bridge]   base_url = "http://{args.host}:{args.port}/v1"', flush=True)
    print('[bridge]   api_key  = "any-non-empty"', flush=True)
    print(f'[bridge]   model    = "{DEFAULT_MODEL}"', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
