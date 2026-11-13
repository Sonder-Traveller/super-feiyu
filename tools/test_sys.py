#!/usr/bin/env python3
"""测任意 system 提示词下的表现。用法: test_sys.py <含提示词的文件>"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:18091/v1/chat/completions"
CASES = ["在吗", "下班了", "好累", "你是谁"]


def call(system: str, user: str) -> str:
    msgs = []
    if system:
        msgs.append({"role": "system", "content": system})
    msgs.append({"role": "user", "content": user})
    body = {"model": "fast-model", "messages": msgs,
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


if __name__ == "__main__":
    p = Path(sys.argv[1])
    system = p.read_text(encoding="utf-8").strip()
    print(f"=== {p.name}  ({len(system)} 字符)\n")
    for u in CASES:
        print(f"你: {u}")
        print(f"它: {call(system, u)}\n")
