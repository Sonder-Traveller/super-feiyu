#!/usr/bin/env python3
"""WorkBuddy 桥接层：把本机已登录的 WorkBuddy 变成 OpenAI 兼容接口。

**独立进程，不属于 super-feiyu 本体。**

    super-feiyu ──▶ http://127.0.0.1:PORT/v1 ──▶ 本桥接 ──▶ WorkBuddy 上游

设计要点：
1. 凭据从 WorkBuddy 桌面端凭据库读取，只在进程内存里，不落盘、不进日志
2. 上游强制流式（stream=true），本桥接把 SSE 聚合成一次性响应
3. 多账号轮换：某个账号限流/失败时自动切下一个
4. 区域感知：cn 与 global 的 token 互不通用，各自绑定自己的 domain
5. 主机白名单：防止凭据被转发到任意地址

用法:
    python3 workbuddy_bridge.py [--port 18091] [--vault PATH] [--region cn|global|auto]
"""
from __future__ import annotations

import argparse
import json
import os
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

VERSION = "0.2.0"

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


# ── 凭据池 ──────────────────────────────────────────────
class Account:
    __slots__ = ("region", "name", "token", "domain", "failures", "cooldown_until")

    def __init__(self, region: str, name: str, token: str, domain: str):
        self.region = region
        self.name = name
        self.token = token
        self.domain = domain
        self.failures = 0
        self.cooldown_until = 0.0

    def available(self) -> bool:
        return time.time() >= self.cooldown_until

    def penalize(self, seconds: float) -> None:
        self.failures += 1
        self.cooldown_until = time.time() + seconds

    def reward(self) -> None:
        self.failures = 0
        self.cooldown_until = 0.0


class AccountPool:
    """多账号池：优先用健康的，失败自动冷却并切换。"""

    def __init__(self, vault: Path, prefer_region: str = "auto"):
        self.vault = vault
        self.prefer_region = prefer_region
        self.accounts: list[Account] = []
        self._lock = threading.Lock()
        self._rr = 0
        self.reload()

    def reload(self) -> None:
        accounts: list[Account] = []
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
                accounts.append(Account(region, name, token, domain))

        # 区域过滤：指定 cn / global 时只保留该区域的账号。
        # 早期版本只做「排序偏好」，轮询仍会轮到 global，导致
        # 11128 request illegal（设备指纹拦截）污染整个请求。
        if self.prefer_region in ("cn", "global"):
            only = [a for a in accounts if a.region == self.prefer_region]
            if only:
                accounts = only

        with self._lock:
            self.accounts = accounts

    def pick(self) -> Account | None:
        with self._lock:
            if not self.accounts:
                return None
            n = len(self.accounts)
            for i in range(n):
                idx = (self._rr + i) % n
                acc = self.accounts[idx]
                if acc.available():
                    self._rr = (idx + 1) % n
                    return acc
            # 全部冷却中：返回冷却最早结束的
            return min(self.accounts, key=lambda a: a.cooldown_until)

    def stats(self) -> list[dict]:
        with self._lock:
            return [
                {
                    "region": a.region,
                    "name": a.name,
                    "domain": a.domain,
                    "failures": a.failures,
                    "available": a.available(),
                    "cooldown_s": max(0, int(a.cooldown_until - time.time())),
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
                piece = (choice.get("delta") or {}).get("content")
                if piece:
                    text_parts.append(piece)

    return {
        "id": "wb-bridge",
        "object": "chat.completion",
        "model": model_name,
        "choices": [
            {
                "index": 0,
                "message": {"role": "assistant", "content": "".join(text_parts)},
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
        for key in ("temperature", "top_p", "max_tokens"):
            if key in req:
                payload[key] = req[key]

        # 多账号重试：失败就换下一个
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
                result = call_upstream(acc, payload, self.timeout_s)
                acc.reward()
                if self.debug:
                    sys.stderr.write(f"[bridge] {key} ok\n")
                self._json(200, result)
                return
            except urllib.error.HTTPError as e:
                detail = ""
                try:
                    detail = e.read().decode("utf-8", errors="replace")[:200]
                except Exception:
                    pass
                last_err = f"{key} HTTP {e.code}: {detail}"
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
    ap.add_argument("--region", default="auto", choices=["auto", "cn", "global"])
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()

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

    if vault is None or not vault.is_dir():
        print(f"[bridge] 凭据目录不存在: {vault}", file=sys.stderr)
        print("[bridge] 用 --vault 指定，或先在本机登录 WorkBuddy 桌面端", file=sys.stderr)
        return 1

    Handler.pool = AccountPool(vault, args.region)
    Handler.timeout_s = args.timeout
    Handler.debug = args.debug

    stats = Handler.pool.stats()
    if not stats:
        print("[bridge] 未找到可用凭据，请求将返回 503", file=sys.stderr)
    else:
        print(f"[bridge] 载入 {len(stats)} 个账号（优先级: {args.region}）:", flush=True)
        for s in stats:
            print(f"[bridge]   {s['region']:6s} {s['name']:20s} {s['domain']}", flush=True)

    try:
        server = ThreadingHTTPServer((args.host, args.port), Handler)
    except OSError as e:
        print(f"[bridge] 无法监听 {args.host}:{args.port}: {e}", file=sys.stderr)
        print("[bridge] 换个端口试试：--port 18092", file=sys.stderr)
        return 1

    print(f"[bridge] listening on http://{args.host}:{args.port}/v1", flush=True)
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
