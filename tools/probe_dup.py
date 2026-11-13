#!/usr/bin/env python3
"""检查：发语音/发图时，模型是否同时给了文字内容。

真实管线要决定：语音已经念了这句话，还要不要再发一遍文字。
"""
from __future__ import annotations

import json
import urllib.request
from pathlib import Path

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
    },
    {
        "type": "function",
        "function": {
            "name": "send_voice",
            "description": "把这句话用语音说出来，代替文字。",
            "parameters": {
                "type": "object",
                "properties": {"text": {"type": "string", "description": "要念出来的话。"}},
                "required": ["text"],
            },
        },
    },
]


def call(system: str, messages: list) -> dict:
    body = {"model": "fast-model", "messages": messages,
            "reasoning_effort": "high", "stream": True,
            "tools": TOOLS, "tool_choice": "auto"}
    req = urllib.request.Request(
        URL, data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        raw = resp.read().decode("utf-8", "replace").strip()
    obj = json.loads(raw)
    ch = (obj.get("choices") or [{}])[0]
    msg = ch.get("message") or {}
    return {"content": (msg.get("content") or "").strip(),
            "tool_calls": msg.get("tool_calls") or []}


system = Path("/tmp/v6.md").read_text(encoding="utf-8")

cases = [
    ("用语音说晚安", "用户要求语音"),
    ("发张照片", "用户要求图"),
    ("累死了", "用户没要求语音"),
]

for user, note in cases:
    r = call(system, [{"role": "system", "content": system},
                      {"role": "user", "content": user}])
    print(f"【{user}】({note})")
    print(f"  content: {r['content']!r}")
    for tc in r["tool_calls"]:
        fn = tc.get("function", {})
        print(f"  tool: {fn.get('name')} {fn.get('arguments')}")
    if r["content"] and r["tool_calls"]:
        print("  ⚠ 同时有文字和工具调用")
    print()
