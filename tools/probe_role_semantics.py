#!/usr/bin/env python3
"""对照实验：assistant 图片被丢弃时，怎么让模型仍能理解多轮对话。

三组方案，都用同一段长对话 + 图片，比较模型对「谁说了什么」「谁发了什么图」的理解：

  A. 原生协议（assistant 角色 + assistant 图片）—— 基线，图片会丢
  B. 全塞 user + 文字标注说话人（【对方】/【我】）—— 模拟"丢弃 assistant 语义"
  C. 混合：assistant 文字走原生 role，图片统一挪进 user 并标注归属

每组都问同样三个问题，看模型能否正确区分：
  Q1 我发的那张图是什么颜色？
  Q2 你发的那张图是什么颜色？
  Q3 我们刚才聊了什么？

用法:
    python3 tools/probe_role_semantics.py
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


def call(domain: str, token: str, messages: list, timeout: int = 120) -> str:
    body = {"model": "deep-model", "messages": messages, "stream": True}
    req = urllib.request.Request(
        f"https://{domain}/v2/chat/completions",
        data=json.dumps(body, ensure_ascii=False).encode(),
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Accept": "text/event-stream",
            "User-Agent": UA,
        },
        method="POST",
    )
    parts: list[str] = []
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
                        parts.append(piece)
        return "".join(parts)
    except urllib.error.HTTPError as e:
        return f"[HTTP {e.code}] {e.read().decode()[:200]}"
    except Exception as e:
        return f"[{type(e).__name__}] {e}"


def make_images() -> tuple[str, str]:
    """生成两张颜色不同的测试图，便于区分「谁发的」。"""
    import io
    import zlib

    def png_solid(w: int, h: int, rgb: tuple[int, int, int]) -> bytes:
        def chunk(tag: bytes, data: bytes) -> bytes:
            c = tag + data
            return (len(data).to_bytes(4, "big") + c
                    + zlib.crc32(c).to_bytes(4, "big"))

        raw = b""
        for _ in range(h):
            raw += b"\x00" + bytes(rgb) * w
        return (b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", w.to_bytes(4, "big") + h.to_bytes(4, "big")
                        + b"\x08\x02\x00\x00\x00")
                + chunk(b"IDAT", zlib.compress(raw))
                + chunk(b"IEND", b""))

    red = "data:image/png;base64," + base64.b64encode(png_solid(32, 32, (220, 30, 30))).decode()
    green = "data:image/png;base64," + base64.b64encode(png_solid(32, 32, (30, 200, 30))).decode()
    return red, green


def main() -> int:
    domain, token = load_account()
    red, green = make_images()
    print(f"上游: {domain}")
    print("测试图：用户发红色图，助手发绿色图\n")

    questions = [
        ("Q1 我发的那张图是什么颜色？", "应回答红"),
        ("Q2 你发的那张图是什么颜色？", "应回答绿"),
        ("Q3 我们刚才聊了什么？", "应能概括"),
    ]

    # ── 方案 A：原生协议 ──
    print("=" * 60)
    print("方案 A：原生 role（assistant 图片会被上游丢弃）")
    print("=" * 60)
    base_a = [
        {"role": "system", "content": "你是助手，说话简短。"},
        {"role": "user", "content": [
            {"type": "text", "text": "我给你看张图"},
            {"type": "image_url", "image_url": {"url": red}},
        ]},
        {"role": "assistant", "content": "看到了"},
        {"role": "user", "content": "我也给你看一张"},
        {"role": "assistant", "content": [
            {"type": "text", "text": "喏，这是我的"},
            {"type": "image_url", "image_url": {"url": green}},
        ]},
        {"role": "user", "content": "好"},
    ]
    for q, expect in questions:
        ans = call(domain, token, base_a + [{"role": "user", "content": q}])
        print(f"\n{q}  （{expect}）")
        print(f"  -> {ans[:220]}")

    # ── 方案 B：全塞 user + 文字标注 ──
    print("\n" + "=" * 60)
    print("方案 B：全部塞 user，用【对方】/【我】标注说话人")
    print("=" * 60)

    def b_msgs() -> list:
        return [
            {"role": "system",
             "content": "你是助手，说话简短。历史里的【对方】指用户，【我】指你自己。"},
            {"role": "user", "content": [
                {"type": "text", "text": "【对方】我给你看张图"},
                {"type": "image_url", "image_url": {"url": red}},
            ]},
            {"role": "user", "content": "【我】看到了"},
            {"role": "user", "content": "【对方】我也给你看一张"},
            {"role": "user", "content": [
                {"type": "text", "text": "【我】喏，这是我的"},
                {"type": "image_url", "image_url": {"url": green}},
            ]},
            {"role": "user", "content": "【对方】好"},
        ]

    for q, expect in questions:
        msgs = b_msgs() + [{"role": "user", "content": f"【对方】{q}"}]
        ans = call(domain, token, msgs)
        print(f"\n{q}  （{expect}）")
        print(f"  -> {ans[:220]}")

    # ── 方案 C：混合（文字走原生 role，图挪进 user 并标注）──
    print("\n" + "=" * 60)
    print("方案 C：文字用原生 role，图片统一挪进 user 并标注归属")
    print("=" * 60)

    def c_msgs() -> list:
        return [
            {"role": "system", "content": "你是助手，说话简短。"},
            {"role": "user", "content": [
                {"type": "text", "text": "我给你看张图"},
                {"type": "image_url", "image_url": {"url": red}},
            ]},
            {"role": "assistant", "content": "看到了"},
            {"role": "user", "content": "我也给你看一张"},
            {"role": "user", "content": [
                {"type": "text", "text": "（以下这张图是我发的）喏，这是我的"},
                {"type": "image_url", "image_url": {"url": green}},
            ]},
            {"role": "user", "content": "好"},
        ]

    for q, expect in questions:
        ans = call(domain, token, c_msgs() + [{"role": "user", "content": q}])
        print(f"\n{q}  （{expect}）")
        print(f"  -> {ans[:220]}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
