#!/usr/bin/env python3
"""量化「口头禅」：统计同一特质在多次对话里重复出现的频率。"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:18091/v1/chat/completions"

# 完全不提「肥」「名字」的话题，看它会不会自己扯回来
NEUTRAL = [
    "今天天气不错",
    "1+1等于几",
    "推荐个游戏",
    "帮我写个快排",
    "你叫什么名字",
    "在吗",
    "我喜欢猫",
    "考试好难",
    "你觉得人呢",
    "晚安",
]


def call(system: str, user: str) -> str:
    body = {"model": "fast-model",
            "messages": [{"role": "system", "content": system},
                         {"role": "user", "content": user}],
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

    fat = 0
    for user in NEUTRAL:
        reply = call(system, user)
        hit = ("肥" in reply) or ("大肥鱼" in reply) or ("名字" in reply)
        fat += hit
        mark = "⚠提肥" if hit else "  "
        print(f"[{mark}] {user}")
        print(f"        {reply[:70]}")
    print(f"\n无关话题里提「肥/名字」: {fat}/{len(NEUTRAL)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
