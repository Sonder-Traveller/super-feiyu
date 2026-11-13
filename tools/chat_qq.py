#!/usr/bin/env python3
"""纯 QQ 式闲聊：短句、碎话、情绪、日常。不涉及技术。"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:18091/v1/chat/completions"

CONVERSATIONS: list[tuple[str, list[str]]] = [
    ("下班", ["下班啦", "累死了", "今天不想动"]),
    ("吃饭", ["吃啥", "随便", "你来选"]),
    ("心情", ["有点烦", "不知道为什么"]),
    ("熬夜", ["又熬夜了", "控制不住"]),
    ("天气", ["今天好冷", "你那儿呢"]),
    ("撒欢", ["嘿嘿", "猜猜我干嘛了"]),
    ("犯困", ["困死了", "但还不想睡"]),
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
