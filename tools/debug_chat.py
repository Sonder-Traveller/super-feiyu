#!/usr/bin/env python3
"""打印桥接层原始响应，排查空回复。"""
from __future__ import annotations

import json
import urllib.request

URL = "http://127.0.0.1:18091/v1/chat/completions"


def raw(prompt: str, model: str) -> None:
    body = json.dumps(
        {
            "model": model,
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": 120,
            "stream": True,
        }
    ).encode()

    req = urllib.request.Request(
        URL,
        data=body,
        headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"},
    )

    print(f"=== model={model}")
    with urllib.request.urlopen(req, timeout=120) as resp:
        print("status:", resp.status)
        n = 0
        for raw_line in resp:
            line = raw_line.decode("utf-8", "replace").rstrip()
            if line:
                print(repr(line[:300]))
                n += 1
            if n > 12:
                print("... (截断)")
                break


if __name__ == "__main__":
    for m in ("deep-model", "fast-model", "auto"):
        try:
            raw("说三个字", m)
        except Exception as e:
            print(f"  {m} 异常: {e}")
        print()
