#!/usr/bin/env python3
"""打印上游原始 SSE，看工具调用到底在哪。"""
from __future__ import annotations

import json
import urllib.request

URL = "http://127.0.0.1:18091/v1/chat/completions"

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "send_photo",
            "description": "发一张自己的照片给对方。返回图片。",
            "parameters": {
                "type": "object",
                "properties": {"scene": {"type": "string", "description": "画面内容"}},
            },
        },
    }
]

SYSTEM = "你是一条蓝色的鲸鱼，住在聊天社区里。要发照片就调用 send_photo。"

body = {
    "model": "fast-model",
    "messages": [
        {"role": "system", "content": SYSTEM},
        {"role": "user", "content": "发张照片看看"},
    ],
    "tools": TOOLS,
    "tool_choice": "auto",
    "reasoning_effort": "high",
    "stream": True,
}

req = urllib.request.Request(
    URL,
    data=json.dumps(body).encode(),
    headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"},
)

with urllib.request.urlopen(req, timeout=180) as resp:
    raw = resp.read().decode("utf-8", "replace")

print(f"=== 原始响应长度: {len(raw)}")
print(f"=== 前 1500 字符:")
print(raw[:1500])
print("\n=== 是否含 tool:")
print("tool_calls" in raw, "tool_call" in raw, "function" in raw)
