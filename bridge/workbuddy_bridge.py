#!/usr/bin/env python3
"""WorkBuddy 桥接层：把本机已登录的 WorkBuddy 变成 OpenAI 兼容接口。

**它不属于 super-feiyu 本体。** 独立进程，super-feiyu 只把它当成一个
普通的 OpenAI 兼容 provider（provider = "openai", base_url = 本服务）。

    super-feiyu ──▶ http://127.0.0.1:18081/v1 ──▶ 本桥接 ──▶ WorkBuddy 上游
       (capability)                                              (copilot.tencent.com
                                                                 或 workbuddy.ai)

设计要点：
1. 凭据从 WorkBuddy 桌面端的凭据文件读取，**只在进程内存里**，不落盘、不进日志
2. 上游只支持流式（stream=true），本桥接把 SSE 聚合成一次性响应
3. 对外接口是 OpenAI 兼容的 /v1/chat/completions（非流式）
4. 区域优先：cn 与 global 的 token 互不通用，按账号实际登录的 domain 路由
5. 不内置任何密钥；全部来自用户机器上已存在的凭据文件

用法:
    python3 workbuddy_bridge.py [--port 18081] [--vault PATH]
"""
from __future__ import annotations

import argparse
import json
import os
import ssl
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

VERSION = "0.1.0"

# 允许桥接走的网关主机（防止 token 被转发到任意地址）
ALLOWED_HOST_SUFFIXES = (
    "copilot.tencent.com",
    "tencentbuddy.com",
    "workbuddy.ai",
    "workbuddy.cn",
    "codebuddy.cn",
    "codebuddy.ai",
)

DEFAULT_MODEL = "fast-model"


# ── 凭据 ────────────────────────────────────────────────
class Credentials:
    """从 WorkBuddy 凭据库读取 token。只在内存中保留。"""

    def __init__(self, vault_dir: Path):
        self.vault_dir = vault_dir

    def _candidates(self):
        """按区域目录遍历凭据文件，cn 优先。"""
        for region in ("cn", "global"):
            d = self.vault_dir / region
            if d.is_dir():
                for f in sorted(d.glob("*.json")):
                    yield region, f

    @staticmethod
    def _parse(path: Path):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            return None
        auth = data.get("auth") or {}
        token = auth.get("accessToken")
        domain = auth.get("domain")
        if not isinstance(token, str) or not token:
            return None
        if not isinstance(domain, str) or not domain:
            return None
        return {"token": token, "domain": domain}

    def best(self):
        """返回 (token, domain)。优先 cn（区域优先于新鲜度）。"""
        for region, path in self._candidates():
            parsed = self._parse(path)
            if parsed:
                return parsed["token"], parsed["domain"]
        return None


def host_allowed(host: str) -> bool:
    return any(host == s or host.endswith("." + s) for s in ALLOWED_HOST_SUFFIXES)


# ── 上游调用 ────────────────────────────────────────────
def call_upstream(token: str, domain: str, payload: dict, timeout: int = 120) -> dict:
    """调用上游流式接口，把 SSE 聚合成一次性响应。"""
    if not host_allowed(domain):
        raise RuntimeError(f"host not allowed: {domain}")

    url = f"https://{domain}/v2/chat/completions"
    body = dict(payload)
    body["stream"] = True  # 上游强制流式

    req = urllib.request.Request(
        url,
        data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Accept": "text/event-stream",
            "User-Agent": f"WorkBuddy/5.8.3 WorkBuddy/5.8.3 CLI/5.8.3",
        },
        method="POST",
    )

    ctx = ssl.create_default_context()
    text_parts: list[str] = []
    usage = None
    model_name = body.get("model", DEFAULT_MODEL)
    finish = "stop"

    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
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
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", errors="replace")[:400]
        raise RuntimeError(f"upstream HTTP {e.code}: {detail}") from None

    text = "".join(text_parts)
    return {
        "id": "wb-bridge",
        "object": "chat.completion",
        "model": model_name,
        "choices": [
            {"index": 0, "message": {"role": "assistant", "content": text},
             "finish_reason": finish}
        ],
        "usage": usage or {},
    }


# ── HTTP 服务 ───────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    server_version = f"WorkBuddyBridge/{VERSION}"
    creds: Credentials = None  # type: ignore
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
        if self.path.rstrip("/") in ("/health", "/v1/health"):
            token_info = self.creds.best() if self.creds else None
            self._json(200, {
                "status": "ok",
                "version": VERSION,
                "credentials": "found" if token_info else "missing",
                # 绝不返回 token 本身
            })
            return
        if self.path.rstrip("/") == "/v1/models":
            self._json(200, {
                "object": "list",
                "data": [{"id": DEFAULT_MODEL, "object": "model", "owned_by": "workbuddy"},
                         {"id": "balanced-model", "object": "model", "owned_by": "workbuddy"},
                         {"id": "deep-model", "object": "model", "owned_by": "workbuddy"}],
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

        cred = self.creds.best() if self.creds else None
        if not cred:
            self._json(503, {"error": {"message": "no workbuddy credentials found"}})
            return
        token, domain = cred

        payload = {
            "model": req.get("model") or DEFAULT_MODEL,
            "messages": req.get("messages") or [],
            "stream": True,
        }
        for key in ("temperature", "top_p", "max_tokens"):
            if key in req:
                payload[key] = req[key]

        try:
            result = call_upstream(token, domain, payload)
        except Exception as e:
            self._json(502, {"error": {"message": str(e)}})
            return

        self._json(200, result)


def main():
    ap = argparse.ArgumentParser(description="WorkBuddy → OpenAI 兼容桥接层")
    ap.add_argument("--port", type=int, default=18081)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--vault", default=None,
                    help="凭据目录，默认 $DSH_HOME/workbuddy-vault")
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()

    if args.vault:
        vault = Path(args.vault)
    else:
        dsh_home = os.environ.get("DSH_HOME")
        if not dsh_home:
            # Windows 与 Linux 的常见位置
            candidates = [
                Path.home() / ".dsh" / "workbuddy-vault",
                Path(os.environ.get("USERPROFILE", "")) / ".dsh" / "workbuddy-vault",
            ]
            vault = next((c for c in candidates if c.is_dir()), candidates[0])
        else:
            vault = Path(dsh_home) / "workbuddy-vault"

    if not vault.is_dir():
        print(f"[bridge] 凭据目录不存在: {vault}", file=sys.stderr)
        print("[bridge] 用 --vault 指定，或先在本机登录 WorkBuddy 桌面端", file=sys.stderr)
        return 1

    Handler.creds = Credentials(vault)
    Handler.debug = args.debug

    cred = Handler.creds.best()
    if cred:
        _, domain = cred
        print(f"[bridge] 凭据已载入，上游域名: {domain}", flush=True)
    else:
        print("[bridge] 未找到可用凭据，请求将返回 503", flush=True)

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"[bridge] listening on http://{args.host}:{args.port}/v1", flush=True)
    print(f"[bridge] 在 super-feiyu 里配置:", flush=True)
    print(f"[bridge]   [capability.chat]", flush=True)
    print(f"[bridge]   provider = \"openai\"", flush=True)
    print(f"[bridge]   base_url = \"http://{args.host}:{args.port}/v1\"", flush=True)
    print(f"[bridge]   api_key  = \"any-non-empty\"", flush=True)
    print(f"[bridge]   model    = \"{DEFAULT_MODEL}\"", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
