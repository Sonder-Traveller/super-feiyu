#!/usr/bin/env python3
"""只看聊天本身：不带工具，纯粹测说话方式。"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:18091/v1/chat/completions"

# 真实对话，带上下文，纯文本
CONVERSATIONS: list[tuple[str, list[str]]] = [
    ("加班", ["还在公司", "又加班", "好烦"]),
    ("周末", ["周末干嘛", "不想出门", "在家躺"]),
    ("失恋", ["我分手了", "心里空空的"]),
    ("无聊", ["好无聊", "陪我聊会"]),
    ("睡觉", ["困了", "先去睡了", "晚安"]),
    ("工作吐槽", ["老板今天又画饼了", "说什么明年上市"]),
    ("吃", ["饿了", "不知道吃什么"]),
]


def call(system: str, messages: list) -> str:
    body = {"model": "fast-model", "messages": messages,
            "reasoning_effort": "high", "stream": True}
    req = urllib.request.Request(
        URL, data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer bridge-local"})
    with urllib.request.urlopen(req, timeout=180) as resp:
        raw = resp.read().decode("utf-8", "replace").strip()
    obj = json.loads(raw)
    ch = (obj.get("choices") or [{}])[0]
    return ((ch.get("message") or {}).get("content") or "").strip()


def main() -> int:
    system = Path(sys.argv[1]).read_text(encoding="utf-8")
    print(f"=== {Path(sys.argv[1]).name} ({len(system)} 字符)\n")
    for title, turns in CONVERSATIONS:
        print(f"── {title}")
        messages = [{"role": "system", "content": system}]
        for user in turns:
            messages.append({"role": "user", "content": user})
            reply = call(system, messages)
            print(f"  你: {user}")
            print(f"  它: {reply}")
            messages.append({"role": "assistant", "content": reply})
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
