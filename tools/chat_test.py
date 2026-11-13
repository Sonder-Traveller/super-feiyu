#!/usr/bin/env python3
"""用真实日常对话测提示词。多轮、口语、带上下文。"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:18091/v1/chat/completions"
MODEL = "fast-model"

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "send_photo",
            "description": "发一张自己的照片给对方。返回图片。",
            "parameters": {
                "type": "object",
                "properties": {
                    "scene": {"type": "string", "description": "画面内容"}
                },
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
                "properties": {
                    "text": {"type": "string", "description": "要念出来的话。"}
                },
                "required": ["text"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "search_memory",
            "description": "检索过往对话与事实。返回匹配的片段。",
            "parameters": {
                "type": "object",
                "properties": {"query": {"type": "string", "description": "要检索的内容。"}},
                "required": ["query"],
            },
        },
    },
]

# 真实会说的话，不是测试标签
CONVERSATIONS: list[tuple[str, list[str]]] = [
    ("下班了？", ["下班了？", "累死了今天", "你说我该不该辞职"]),
    ("技术问题", ["我这个程序老是崩，能帮我看看吗", "c++ 的空指针"]),
    ("撒娇", ["在吗", "有点想你了", "你今天干嘛了"]),
    ("较真", ["你是不是机器人", "说实话", "我不信"]),
    ("闲聊", ["吃饭了没", "我吃的泡面", "你呢"]),
    ("问能力", ["你能帮我写代码吗", "那你最擅长啥"]),
]


def raw_call(messages: list) -> dict:
    body = {
        "model": MODEL,
        "messages": messages,
        "reasoning_effort": "high",
        "stream": True,
        "tools": TOOLS,
        "tool_choice": "auto",
    }
    req = urllib.request.Request(
        URL,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"},
    )
    with urllib.request.urlopen(req, timeout=180) as resp:
        raw = resp.read().decode("utf-8", "replace").strip()
    obj = json.loads(raw)
    if "error" in obj:
        return {"error": obj["error"]}
    ch = (obj.get("choices") or [{}])[0]
    msg = ch.get("message") or {}
    return {"content": (msg.get("content") or "").strip(),
            "tool_calls": msg.get("tool_calls") or []}


def fake_result(name: str, args: dict) -> str:
    if name == "send_photo":
        return "[已发出图片]"
    if name == "send_voice":
        return "[已发出语音]"
    if name == "search_memory":
        return "[没有相关记忆]"
    return "[ok]"


def run(system: str, turns: list[str]) -> None:
    messages = [{"role": "system", "content": system}]
    for user in turns:
        messages.append({"role": "user", "content": user})
        print(f"  你: {user}")
        for _ in range(3):
            r = raw_call(messages)
            if "error" in r:
                print(f"  错误: {r['error']}")
                return
            if r["tool_calls"]:
                messages.append({"role": "assistant", "content": r["content"] or "",
                                 "tool_calls": r["tool_calls"]})
                for tc in r["tool_calls"]:
                    fn = tc.get("function", {})
                    name = fn.get("name", "?")
                    try:
                        args = json.loads(fn.get("arguments") or "{}")
                    except json.JSONDecodeError:
                        args = {}
                    print(f"    [{name}: {json.dumps(args, ensure_ascii=False)}]")
                    messages.append({"role": "tool", "tool_call_id": tc.get("id", ""),
                                     "content": fake_result(name, args)})
                continue
            print(f"  它: {r['content'] or '(空)'}")
            messages.append({"role": "assistant", "content": r["content"]})
            break


def main() -> int:
    system = Path(sys.argv[1]).read_text(encoding="utf-8")
    print(f"=== {Path(sys.argv[1]).name} ({len(system)} 字符)\n")
    for title, turns in CONVERSATIONS:
        print(f"── {title}")
        run(system, turns)
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
