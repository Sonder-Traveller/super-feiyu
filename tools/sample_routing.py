#!/usr/bin/env python3
"""多次采样 fast-model，确认它稳定路由到 deepseek。"""
from __future__ import annotations

import json
import urllib.request
from collections import Counter

URL = "http://127.0.0.1:18091/v1/chat/completions"


def once(model: str) -> str:
    body = json.dumps(
        {
            "model": model,
            "messages": [{"role": "user", "content": "只回答：好"}],
            "stream": True,
        }
    ).encode()
    req = urllib.request.Request(
        URL,
        data=body,
        headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"},
    )
    with urllib.request.urlopen(req, timeout=90) as resp:
        raw = resp.read().decode("utf-8", "replace").strip()
    obj = json.loads(raw)
    return obj.get("model", "?")


if __name__ == "__main__":
    for model in ("fast-model", "deep-model"):
        seen: Counter[str] = Counter()
        for _ in range(6):
            try:
                seen[once(model)] += 1
            except Exception as e:
                seen[f"ERR:{type(e).__name__}"] += 1
        print(f"{model:14} -> {dict(seen)}")
