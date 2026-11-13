#!/usr/bin/env python3
"""验证空人格时的对话表现：不带 system 消息直接问。"""
from __future__ import annotations

import json
import urllib.request

URL = "http://127.0.0.1:18091/v1/chat/completions"

CASES = ["在吗", "下班了", "好累", "你是谁", "1+1等于几"]


def call(user: str) -> str:
    body = {
        "model": "fast-model",
        "messages": [{"role": "user", "content": user}],
        "reasoning_effort": "high",
        "stream": True,
    }
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
    print("=== 无人格（无 system 消息）时的表现\n")
    for u in CASES:
        print(f"你: {u}")
        print(f"它: {call(u)}\n")
