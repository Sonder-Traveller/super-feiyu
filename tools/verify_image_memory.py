#!/usr/bin/env python3
"""验证「模型能重新看到记忆里的图」。

模拟 pipeline 的行为：把历史消息里的图片路径转成 data URI，
连同文字一起发给模型，看它能不能描述出来。

用法:
    python3 tools/verify_image_memory.py
"""
from __future__ import annotations

import base64
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

BRIDGE = "http://127.0.0.1:18091/v1"
MODEL = "deep-model"


def data_uri(path: Path) -> str:
    raw = path.read_bytes()
    ext = path.suffix.lower()
    mime = {".png": "image/png", ".jpg": "image/jpeg",
            ".jpeg": "image/jpeg", ".gif": "image/gif"}.get(ext, "image/png")
    return f"data:{mime};base64,{base64.b64encode(raw).decode()}"


def ask(messages: list) -> str:
    body = json.dumps({
        "model": MODEL,
        "messages": messages,
        "stream": False,
    }).encode()
    req = urllib.request.Request(
        f"{BRIDGE}/chat/completions",
        data=body,
        headers={"Content-Type": "application/json", "Authorization": "Bearer x"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            d = json.loads(r.read().decode())
        return d["choices"][0]["message"]["content"] or ""
    except urllib.error.HTTPError as e:
        return f"[HTTP {e.code}] {e.read().decode()[:300]}"
    except Exception as e:
        return f"[{type(e).__name__}] {e}"


def main() -> int:
    img = Path("data/memory/portraits/placeholder.png")
    if not img.exists():
        print(f"[x] 找不到测试图: {img}", file=sys.stderr)
        return 1

    uri = data_uri(img)
    print(f"测试图: {img} ({img.stat().st_size} 字节)")
    print(f"data URI 长度: {len(uri)}")
    print()

    # 场景：历史里有 assistant 发过这张图，用户现在问「描述一下这张图」
    messages = [
        {"role": "system", "content": "你叫大肥鱼，说话简短。"},
        # 历史：assistant 发过图（模拟记忆里带 image_paths）
        {"role": "assistant", "content": [
            {"type": "text", "text": "喏"},
            {"type": "image_url", "image_url": {"url": uri}},
        ]},
        {"role": "user", "content": "描述一下这张图"},
    ]

    print("=== 历史带图 + 用户问「描述一下这张图」 ===")
    answer = ask(messages)
    print(f"模型回答: {answer[:300]}")
    print()

    keywords = ["蓝", "圆", "眼睛", "鲸", "blue", "circle"]
    hit = [k for k in keywords if k in answer.lower()]
    ok = bool(hit) and "看不到" not in answer and "不知道" not in answer

    print(f"命中图像特征: {hit if hit else '无'}")
    print(f"是否回避（看不到/不知道）: {'是' if ('看不到' in answer or '不知道' in answer) else '否'}")
    print()
    print("[✓] 模型能看到记忆里的图" if ok else "[✗] 模型仍然看不到")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
