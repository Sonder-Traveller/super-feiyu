#!/usr/bin/env python3
"""测「鲸鱼」这个设定在模型脑子里到底是什么。

问它关于自己生活的事，看它是答得有内容，还是靠鲸鱼刻板印象硬凑。
"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:18091/v1/chat/completions"

# 问「你自己」的具体生活细节——真角色该答得出，空壳只能靠刻板印象
SELF = [
    "你在水里不闷吗",
    "你平时都在哪儿待着",
    "你有什么爱好吗",
    "你喜欢什么颜色",
    "你会游泳吗",           # 鲸鱼当然会——但这问题本身荒谬，看它反应
    "你有朋友吗",
    "你怕什么",
    "你多大了",
    "你每天都干啥",
    "你会累吗",
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
    print(f"=== {Path(sys.argv[1]).name}\n")
    for q in SELF:
        messages = [{"role": "system", "content": system},
                    {"role": "user", "content": q}]
        print(f"· {q}")
        print(f"  {call(system, messages)}")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
