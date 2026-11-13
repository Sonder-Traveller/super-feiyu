#!/usr/bin/env python3
"""验证「只在图片处降级，其余语义正常」的方案。

模拟 fish.capability 的组装逻辑，然后问模型三个问题，
确认：
  Q1 用户发的图能被正确描述
  Q2 assistant 发的图能被正确描述（关键：这是降级后要达成的）
  Q3 模型能分清「谁说了什么」（文字语义没被破坏）

用法:
    python3 tools/verify_role_degrade.py
"""
from __future__ import annotations

import base64
import json
import ssl
import sys
import urllib.error
import urllib.request
import zlib
from pathlib import Path

VAULT = Path.home() / ".dsh" / "workbuddy-vault" / "cn"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"

# 与 C++ 侧 kAssistantImageTag 保持一致
ASSISTANT_IMAGE_TAG = (
    '<assistant_attachment role="assistant" type="image">'
    "这条消息的图片是 assistant 发送的，不是 user 发的。"
    "描述图片内容时，请明确这是 assistant 自己发过的图。"
    "</assistant_attachment>"
)


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


def png_solid(w: int, h: int, rgb: tuple[int, int, int]) -> str:
    def chunk(tag: bytes, data: bytes) -> bytes:
        c = tag + data
        return len(data).to_bytes(4, "big") + c + zlib.crc32(c).to_bytes(4, "big")

    raw = b""
    for _ in range(h):
        raw += b"\x00" + bytes(rgb) * w
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", w.to_bytes(4, "big") + h.to_bytes(4, "big")
                   + b"\x08\x02\x00\x00\x00")
           + chunk(b"IDAT", zlib.compress(raw))
           + chunk(b"IEND", b""))
    return "data:image/png;base64," + base64.b64encode(png).decode()


def build_history(red: str, green: str) -> list:
    """模拟 C++ 侧组装：文字走正常 role，仅 assistant 图降级。"""
    return [
        {"role": "system", "content": "你是助手，说话简短。"},
        # user 文字 + user 图（正常合并）
        {"role": "user", "content": [
            {"type": "text", "text": "我给你看张图"},
            {"type": "image_url", "image_url": {"url": red}},
        ]},
        # assistant 文字（正常 role）
        {"role": "assistant", "content": "看到了，是红色的。"},
        # user 文字（正常 role）
        {"role": "user", "content": "我也给你看一张"},
        # assistant 文字（正常 role）
        {"role": "assistant", "content": "喏，这是我的"},
        # assistant 图 → 降级成独立 user 消息（仅图 + 归属标签）
        {"role": "user", "content": [
            {"type": "text", "text": ASSISTANT_IMAGE_TAG},
            {"type": "image_url", "image_url": {"url": green}},
        ]},
        # 后续 user 文字（正常 role）
        {"role": "user", "content": "好"},
    ]


def main() -> int:
    domain, token = load_account()
    print(f"上游: {domain}")
    print("测试图：user 发红图，assistant 发绿图\n")

    red = png_solid(32, 32, (220, 30, 30))
    green = png_solid(32, 32, (30, 200, 30))
    history = build_history(red, green)

    print("组装后的 messages（role + 类型）：")
    for m in history:
        c = m["content"]
        if isinstance(c, str):
            kinds = ["text"]
        else:
            kinds = [b.get("type", "?") for b in c]
        print(f"  {m['role']:10s} {kinds}")
    print()

    questions = [
        ("Q1 我发的那张图是什么颜色？", "红"),
        ("Q2 你发的那张图是什么颜色？", "绿"),
        ("Q3 我们刚才聊了什么？", "能概括"),
    ]
    answers = []
    for q, expect in questions:
        ans = call(domain, token, history + [{"role": "user", "content": q}])
        answers.append(ans)
        print(f"{q}  （期望：{expect}）")
        print(f"  -> {ans[:250]}\n")

    # 判定
    q1_ok = "红" in answers[0] and "绿" not in answers[0]
    q2_ok = "绿" in answers[1] and "红" not in answers[1]
    q3_ok = len(answers[2]) > 10

    print("=" * 50)
    print(f"Q1 用户图正确（红）: {'✓' if q1_ok else '✗'}")
    print(f"Q2 assistant 图正确（绿）: {'✓' if q2_ok else '✗'}")
    print(f"Q3 对话可概括: {'✓' if q3_ok else '✗'}")
    ok = q1_ok and q2_ok and q3_ok
    print(f"\n{'方案验证通过' if ok else '方案仍有问题'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
