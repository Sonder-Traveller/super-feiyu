#!/usr/bin/env python3
"""测试桥接层对话，同时兼容两种返回：

  1. SSE 流式（data: {...} 逐行）
  2. 一次性完整 JSON（桥接层实际行为：无视 stream=true）
"""
from __future__ import annotations

import json
import sys
import urllib.request

URL = "http://127.0.0.1:18091/v1/chat/completions"


def ask(prompt: str, model: str = "deep-model") -> tuple[str, str]:
    """返回 (回复文本, 实际模型)。"""
    body = json.dumps(
        {
            "model": model,
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": 150,
            "stream": True,
        }
    ).encode()

    req = urllib.request.Request(
        URL,
        data=body,
        headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"},
    )

    with urllib.request.urlopen(req, timeout=120) as resp:
        raw = resp.read().decode("utf-8", "replace")

    # 情况 2：整体是一个 JSON 对象
    stripped = raw.strip()
    if stripped.startswith("{"):
        obj = json.loads(stripped)
        model_used = obj.get("model", "?")
        for ch in obj.get("choices", []):
            msg = ch.get("message") or {}
            content = msg.get("content")
            if content:
                return content, model_used
        return "", model_used

    # 情况 1：SSE
    out: list[str] = []
    model_used = "?"
    for line in raw.splitlines():
        line = line.strip()
        if not line.startswith("data:"):
            continue
        payload = line[5:].strip()
        if payload == "[DONE]":
            break
        try:
            obj = json.loads(payload)
        except json.JSONDecodeError:
            continue
        model_used = obj.get("model", model_used)
        for ch in obj.get("choices", []):
            piece = (ch.get("delta") or {}).get("content")
            if piece:
                out.append(piece)
    return "".join(out), model_used


if __name__ == "__main__":
    prompt = sys.argv[1] if len(sys.argv) > 1 else "用一句话介绍你自己"
    model = sys.argv[2] if len(sys.argv) > 2 else "deep-model"
    text, used = ask(prompt, model)
    print(f"模型: {used}")
    print(f"回复: {text or '(空)'}")
    print(f"长度: {len(text)}")
