#!/usr/bin/env python3
"""验证模型是否支持读图（多模态）。

用一张有明显特征的图（纯色 + 形状），问模型图里是什么。
支持视觉的模型能说出颜色/形状；不支持的会拒答或说看不到。

用法:
    python3 tools/vision_probe.py --model balanced-model
    python3 tools/vision_probe.py --model balanced-model --image path.png
"""
from __future__ import annotations

import argparse
import base64
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path


def build_payload(model: str, b64: str, mime: str, question: str) -> dict:
    return {
        "model": model,
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": question},
                    {
                        "type": "image_url",
                        "image_url": {"url": f"data:{mime};base64,{b64}"},
                    },
                ],
            }
        ],
        "stream": False,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="验证模型读图能力")
    ap.add_argument("--base", default="http://127.0.0.1:18091/v1")
    ap.add_argument("--model", default="balanced-model")
    ap.add_argument("--image", default="data/memory/portraits/placeholder.png")
    ap.add_argument("--question", default="这张图里是什么颜色、什么形状？一句话回答。")
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    img = Path(args.image)
    if not img.exists():
        print(f"[x] 图片不存在: {img}", file=sys.stderr)
        return 1
    raw = img.read_bytes()
    b64 = base64.b64encode(raw).decode()
    mime = "image/png" if img.suffix.lower() == ".png" else "image/jpeg"

    print(f"模型: {args.model}")
    print(f"图片: {img} ({len(raw)} 字节 → base64 {len(b64)})")
    print()

    body = json.dumps(build_payload(args.model, b64, mime, args.question)).encode()
    req = urllib.request.Request(
        f"{args.base}/chat/completions",
        data=body,
        headers={"Content-Type": "application/json", "Authorization": "Bearer probe"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=args.timeout) as r:
            d = json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        print(f"[x] HTTP {e.code}")
        try:
            print(e.read().decode()[:500])
        except Exception:
            pass
        return 1
    except Exception as e:
        print(f"[x] {type(e).__name__}: {e}", file=sys.stderr)
        return 1

    real_model = d.get("model", "?")
    content = ""
    try:
        content = d["choices"][0]["message"]["content"] or ""
    except Exception:
        pass

    print(f"实际模型: {real_model}")
    print(f"回答: {content[:400]}")
    print()

    # 粗判：回答里出现颜色/形状关键词，认为能看图
    keywords = ["蓝", "蓝鲸", "圆", "球", "眼睛", "鲸", "图", "blue", "circle", "round"]
    hit = [k for k in keywords if k in content.lower()]
    if not content:
        print("[?] 没有文本回答，无法判断")
    elif hit:
        print(f"[✓] 疑似支持读图（命中关键词: {', '.join(hit)}）")
    else:
        print("[?] 回答里没有明显的图像特征词，需要人工确认")
        print("    如果回答类似「我看不到图片」「请描述一下」，则说明不支持视觉")
    return 0


if __name__ == "__main__":
    sys.exit(main())
