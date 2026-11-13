#!/usr/bin/env python3
"""直接探测 WorkBuddy 上游 /v2/chat/completions 的协议能力。

目的：确认它到底接受哪些多模态格式，以及 assistant 角色的图是否被支持。
绕开桥接，直接打上游，避免中间层掩盖问题。

用法:
    python3 tools/probe_upstream_protocol.py
"""
from __future__ import annotations

import base64
import json
import ssl
import sys
import urllib.error
import urllib.request
from pathlib import Path

VAULT = Path.home() / ".dsh" / "workbuddy-vault" / "cn"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"


def load_account() -> tuple[str, str]:
    """从 vault 取第一个可用的 cn 账号 (domain, token)。"""
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


def call(domain: str, token: str, payload: dict, timeout: int = 90) -> tuple[int, str]:
    body = dict(payload)
    body["stream"] = True
    req = urllib.request.Request(
        f"https://{domain}/v2/chat/completions",
        data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Accept": "text/event-stream",
            "User-Agent": UA,
        },
        method="POST",
    )
    text_parts: list[str] = []
    try:
        with urllib.request.urlopen(req, timeout=timeout,
                                    context=ssl.create_default_context()) as resp:
            for raw in resp:
                line = raw.decode("utf-8", errors="replace").strip()
                if not line.startswith("data:"):
                    continue
                chunk = line[5:].strip()
                if chunk == "[DONE]":
                    break
                try:
                    d = json.loads(chunk)
                except Exception:
                    continue
                for ch in d.get("choices") or []:
                    piece = (ch.get("delta") or {}).get("content")
                    if piece:
                        text_parts.append(piece)
        return 200, "".join(text_parts)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", errors="replace")[:400]
    except Exception as e:
        return 0, f"{type(e).__name__}: {e}"


def main() -> int:
    domain, token = load_account()
    print(f"上游: {domain}")
    print()

    img = Path("data/memory/portraits/placeholder.png")
    if not img.exists():
        print(f"[x] 缺测试图 {img}", file=sys.stderr)
        return 1
    uri = f"data:image/png;base64,{base64.b64encode(img.read_bytes()).decode()}"
    print(f"测试图: {img} ({img.stat().st_size}B), data URI {len(uri)} 字符")
    print()

    cases = [
        ("纯文本基线", {
            "model": "deep-model",
            "messages": [{"role": "user", "content": "说个数字"}],
        }),
        ("user 带图（OpenAI image_url）", {
            "model": "deep-model",
            "messages": [{"role": "user", "content": [
                {"type": "text", "text": "图里什么颜色"},
                {"type": "image_url", "image_url": {"url": uri}},
            ]}],
        }),
        ("user 带图（简写 url 字符串）", {
            "model": "deep-model",
            "messages": [{"role": "user", "content": [
                {"type": "text", "text": "图里什么颜色"},
                {"type": "image_url", "image_url": uri},
            ]}],
        }),
        ("assistant 带图", {
            "model": "deep-model",
            "messages": [
                {"role": "assistant", "content": [
                    {"type": "text", "text": "喏"},
                    {"type": "image_url", "image_url": {"url": uri}},
                ]},
                {"role": "user", "content": "什么颜色"},
            ],
        }),
        ("user 带图（image 类型）", {
            "model": "deep-model",
            "messages": [{"role": "user", "content": [
                {"type": "text", "text": "图里什么颜色"},
                {"type": "image", "image": uri},
            ]}],
        }),
    ]

    results = []
    for label, payload in cases:
        code, out = call(domain, token, payload)
        ok = code == 200 and out.strip()
        results.append((label, code, out))
        print(f"=== {label} ===")
        print(f"  HTTP {code}")
        print(f"  {out[:200]}")
        print()

    print("=== 汇总 ===")
    for label, code, out in results:
        # 判断是否真的看到了图：回答里出现图像特征词
        saw = any(k in out for k in ["蓝", "圆", "眼睛", "blue", "circle"])
        deny = any(k in out for k in ["没看到", "看不到", "没图", "没收到", "无法看到"])
        status = "看图成功" if saw else ("拒绝/看不到" if deny else "未判断")
        print(f"  {label:32s} HTTP {code:3d}  {status}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
