#!/usr/bin/env python3
"""长对话测试：一段连续十几轮，看会不会漂、会不会重复、会不会变机器。"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:18091/v1/chat/completions"

# 一段真实的长对话：从下班聊到工作、情绪、再聊回技术
LONG: list[str] = [
    "下班了",
    "今天开了一天会",
    "全是没用的会",
    "你说这些会到底有啥意义",
    "我在想是不是该换个工作",
    "但我又怕换了更差",
    "你说是稳定重要还是开心重要",
    "算了不说这个了",
    "你平时都干嘛",
    "你会无聊吗",
    "我有点羡慕你",
    "不用上班",
    "不过我猜你也有你的难处",
    "对了你会写代码吧",
    "帮我看看这个报错是啥意思：segmentation fault",
    "懂了，谢了",
    "不早了，我睡了",
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

    messages = [{"role": "system", "content": system}]
    replies: list[str] = []

    for i, user in enumerate(LONG, 1):
        messages.append({"role": "user", "content": user})
        reply = call(system, messages)
        replies.append(reply)
        print(f"[{i:2}] 你: {user}")
        print(f"     它: {reply}\n")
        messages.append({"role": "assistant", "content": reply})

    # 简单统计
    print("=" * 50)
    all_text = "".join(replies)
    print(f"总字符: {len(all_text)}  平均每条: {len(all_text)//max(1,len(replies))}")
    print(f"提「大肥鱼/肥」次数: {all_text.count('肥')}")
    print(f"反问号「？」次数: {all_text.count('？')}")

    # 检查重复短语
    from collections import Counter
    phrases = [r[:6] for r in replies if len(r) >= 6]
    dup = [p for p, c in Counter(phrases).items() if c > 1]
    if dup:
        print(f"开头重复: {dup}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
