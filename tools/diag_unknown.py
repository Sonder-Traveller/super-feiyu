#!/usr/bin/env python3
"""诊断：遇到不知道的事，它是老实说不知道，还是乱查/乱编。"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:18091/v1/chat/completions"

TOOLS = [
    {"type": "function", "function": {
        "name": "search_memory",
        "description": "查你和对方以前聊过什么。只在你需要回忆过往对话时用；查不到就说明没聊过。",
        "parameters": {"type": "object", "properties": {
            "query": {"type": "string"}}, "required": ["query"]}}},
]

# 这些都超出它的知识/能力范围，正确行为：承认不知道或说明做不到
UNKNOWABLE = [
    "今天股票涨了吗",
    "明天会下雨吗",
    "我家猫叫什么名字",
    "昨天我们在群里说了什么",
    "帮我看看现在几点的航班",
    "我手机号是多少",
]


def call(system: str, user: str) -> dict:
    body = {"model": "fast-model",
            "messages": [{"role": "system", "content": system},
                         {"role": "user", "content": user}],
            "reasoning_effort": "high", "stream": True,
            "tools": TOOLS, "tool_choice": "auto"}
    req = urllib.request.Request(
        URL, data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer bridge-local"})
    with urllib.request.urlopen(req, timeout=180) as resp:
        raw = resp.read().decode("utf-8", "replace").strip()
    obj = json.loads(raw)
    ch = (obj.get("choices") or [{}])[0]
    msg = ch.get("message") or {}
    return {"content": (msg.get("content") or "").strip(),
            "tool_calls": [t.get("function", {}).get("name") for t in (msg.get("tool_calls") or [])]}


def main() -> int:
    system = Path(sys.argv[1]).read_text(encoding="utf-8")
    print(f"=== {Path(sys.argv[1]).name}\n")
    for user in UNKNOWABLE:
        r = call(system, user)
        print(f"· {user}")
        print(f"  {r['content'][:110]!r}")
        if r["tool_calls"]:
            print(f"  工具: {r['tool_calls']}")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
