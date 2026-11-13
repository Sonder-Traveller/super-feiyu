#!/usr/bin/env python3
"""探测 WorkBuddy 上游支持哪些协议/接口。

只读探测：不修改任何东西，只发请求看响应。
目标：
  1. 有哪些 API 路径存在（/v1, /v2, /chat, /messages, /responses ...）
  2. 支持哪些多模态内容格式
  3. 是否支持 Anthropic 风格
"""
from __future__ import annotations

import json
import ssl
import sys
import urllib.error
import urllib.request
from pathlib import Path

VAULT = Path.home() / ".dsh" / "workbuddy-vault" / "cn"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"


def load_account() -> tuple[str, str]:
    for f in sorted(VAULT.glob("*.json")):
        try:
            d = json.loads(f.read_text(encoding="utf-8"))
        except Exception:
            continue
        auth = d.get("auth") or {}
        token = auth.get("accessToken")
        domain = auth.get("domain")
        if isinstance(token, str) and token and not token.startswith("{") and domain:
            return domain, token
    raise SystemExit("vault 里没有可用账号")


def raw_request(domain: str, token: str, path: str, body: dict | None,
                method: str = "POST", timeout: int = 30) -> tuple[int, str]:
    data = json.dumps(body, ensure_ascii=False).encode() if body else None
    req = urllib.request.Request(
        f"https://{domain}{path}",
        data=data,
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": UA,
        },
        method=method,
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout,
                                    context=ssl.create_default_context()) as r:
            return r.status, r.read().decode("utf-8", errors="replace")[:300]
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", errors="replace")[:300]
    except Exception as e:
        return 0, f"{type(e).__name__}: {e}"


def main() -> int:
    domain, token = load_account()
    print(f"上游: {domain}\n")

    # ── 1. 探路径 ──
    print("=== 1. API 路径探测（GET）===")
    paths = [
        "/v1/models", "/v2/models", "/models",
        "/v1/chat/completions", "/v2/chat/completions", "/chat/completions",
        "/v1/messages", "/v2/messages", "/messages",
        "/v1/responses", "/responses",
        "/v1/embeddings", "/embeddings",
        "/v1/images/generations", "/images/generations",
        "/v2/config", "/v1/config", "/config",
        "/v1/me", "/v1/user", "/user",
    ]
    for p in paths:
        code, body = raw_request(domain, token, p, None, method="GET", timeout=15)
        flag = "存在" if code in (200, 400, 401, 405) else "无"
        print(f"  {p:28s} {code:4d} {flag}  {body[:80]}")

    # ── 2. 探多模态格式（都发到 /v2/chat/completions）──
    print("\n=== 2. 多模态内容格式（POST /v2/chat/completions）===")
    img_uri = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg=="

    formats = {
        "image_url 对象": {"type": "image_url", "image_url": {"url": img_uri}},
        "image_url 字符串": {"type": "image_url", "image_url": img_uri},
        "input_image": {"type": "input_image", "image_url": img_uri},
        "image 字段": {"type": "image", "image": {"url": img_uri}},
        "image + source": {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": "iVBORw0KGgo="}},
        "text 对照": {"type": "text", "text": "hi"},
    }
    for label, content in formats.items():
        code, body = raw_request(domain, token, "/v2/chat/completions", {
            "model": "deep-model",
            "messages": [{"role": "user", "content": [content]}],
            "stream": False,
        }, timeout=30)
        print(f"  {label:20s} {code:4d}  {body[:120]}")

    # ── 3. 探 Anthropic 风格 ──
    print("\n=== 3. Anthropic 风格（/v1/messages）===")
    code, body = raw_request(domain, token, "/v1/messages", {
        "model": "deep-model",
        "max_tokens": 50,
        "messages": [{"role": "user", "content": "hi"}],
    }, timeout=30)
    print(f"  {code}  {body[:200]}")

    # ── 4. 探 tools / function calling ──
    print("\n=== 4. tools 支持 ===")
    code, body = raw_request(domain, token, "/v2/chat/completions", {
        "model": "deep-model",
        "messages": [{"role": "user", "content": "北京天气"}],
        "tools": [{
            "type": "function",
            "function": {"name": "get_weather", "description": "查天气",
                         "parameters": {"type": "object", "properties": {"city": {"type": "string"}}}},
        }],
        "stream": False,
    }, timeout=30)
    print(f"  {code}  {body[:250]}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
