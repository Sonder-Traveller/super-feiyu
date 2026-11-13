#!/usr/bin/env python3
"""排查工具结果回填：workbuddy 认不认 role=tool？"""
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

SYSTEM = "你是一条蓝色的鲸鱼，住在聊天软件里。别人叫你大肥鱼。\n\n要发照片就调用 send_photo。"


def post(messages: list, tools=None, note: str = "") -> dict:
    body = {"model": "fast-model", "messages": messages, "reasoning_effort": "high", "stream": True}
    if tools:
        body["tools"] = tools
        body["tool_choice"] = "auto"
    req = urllib.request.Request(
        URL,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"},
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            raw = resp.read().decode("utf-8", "replace").strip()
    except Exception as e:
        return {"error": str(e)}
    obj = json.loads(raw)
    if "error" in obj:
        return {"error": obj["error"]}
    ch = (obj.get("choices") or [{}])[0]
    msg = ch.get("message") or {}
    return {
        "content": (msg.get("content") or "").strip(),
        "tool_calls": msg.get("tool_calls") or [],
        "finish": ch.get("finish_reason"),
    }


print("── 第 1 轮：只给问题，看它调不调工具")
msgs = [
    {"role": "system", "content": SYSTEM},
    {"role": "user", "content": "发张照片看看"},
]
r1 = post(msgs, TOOLS)
print(f"  content={r1.get('content')!r}")
print(f"  tool_calls={len(r1.get('tool_calls') or [])}")
print(f"  finish={r1.get('finish')}")
if r1.get("tool_calls"):
    for tc in r1["tool_calls"]:
        print(f"    {tc.get('function', {}).get('name')} {tc.get('function', {}).get('arguments')}")
        print(f"    id={tc.get('id')}")

if not r1.get("tool_calls"):
    raise SystemExit("没调工具，无法继续测回填")

tc = r1["tool_calls"][0]

print("\n── 第 2 轮 A：用 role=tool 回填")
msgs_a = msgs + [
    {"role": "assistant", "content": "", "tool_calls": [tc]},
    {"role": "tool", "tool_call_id": tc.get("id", ""), "content": "[已发送图片]"},
]
ra = post(msgs_a, TOOLS)
print(f"  content={ra.get('content')!r}")
print(f"  tool_calls={len(ra.get('tool_calls') or [])}")
print(f"  finish={ra.get('finish')}")

print("\n── 第 2 轮 B：用 role=user 回填")
msgs_b = msgs + [
    {"role": "assistant", "content": "", "tool_calls": [tc]},
    {"role": "user", "content": "[系统：send_photo 已执行，图片已发给对方]"},
]
rb = post(msgs_b, TOOLS)
print(f"  content={rb.get('content')!r}")
print(f"  tool_calls={len(rb.get('tool_calls') or [])}")
print(f"  finish={rb.get('finish')}")

print("\n── 第 2 轮 C：不回填，直接再问（不带工具）")
msgs_c = msgs
rc = post(msgs_c, None)
print(f"  content={rc.get('content')!r}")
print(f"  finish={rc.get('finish')}")
