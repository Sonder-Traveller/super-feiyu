#!/usr/bin/env python3
"""对比：不传 reasoning 参数 vs 传 reasoning_effort，看上游行为差异。"""
from __future__ import annotations

import json
import urllib.request

URL = "http://127.0.0.1:18091/v1/chat/completions"


def call(extra: dict, model: str = "deep-model") -> dict:
    body = {
        "model": model,
        "messages": [{"role": "user", "content": "1+1等于几？只回答数字"}],
        "max_tokens": 50,
        "stream": True,
    }
    body.update(extra)

    req = urllib.request.Request(
        URL,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"},
    )
    with urllib.request.urlopen(req, timeout=120) as resp:
        raw = resp.read().decode("utf-8", "replace")

    s = raw.strip()
    if s.startswith("{"):
        obj = json.loads(s)
        ch = (obj.get("choices") or [{}])[0]
        msg = ch.get("message") or {}
        return {
            "model": obj.get("model"),
            "content": msg.get("content"),
            "reasoning": msg.get("reasoning_content") or msg.get("reasoning"),
            "usage": obj.get("usage"),
        }
    return {"raw": raw[:300]}


if __name__ == "__main__":
    cases = [
        ("不传 reasoning", {}),
        ("reasoning_effort=low", {"reasoning_effort": "low"}),
        ("reasoning_effort=high", {"reasoning_effort": "high"}),
        ("thinking 对象", {"thinking": {"type": "enabled", "budget_tokens": 1024}}),
    ]
    for name, extra in cases:
        try:
            r = call(extra)
            print(f"=== {name}")
            print(f"  model: {r.get('model')}")
            print(f"  content: {str(r.get('content'))[:80]}")
            rc = r.get("reasoning")
            print(f"  reasoning: {'有 ' + str(len(rc)) + ' 字符' if rc else '无'}")
            u = r.get("usage") or {}
            print(f"  completion_tokens: {u.get('completion_tokens')}")
            print(f"  details: {u.get('completion_tokens_details')}")
        except Exception as e:
            print(f"=== {name} 异常: {e}")
        print()
