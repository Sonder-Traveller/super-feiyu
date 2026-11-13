#!/usr/bin/env python3
"""验证真实模型 id + reasoning_effort 能否在桥接层生效。

桥接层是 OpenAI 兼容接口，模型 id 用目录里的真名（不是 deep-model 这类抽象名）。
"""
from __future__ import annotations

import json
import urllib.request

URL = "http://127.0.0.1:18091/v1/chat/completions"
MODEL = "deepseek-v4.1-flash"


def call(extra: dict, prompt: str = "9.11 和 9.9 哪个大？") -> dict:
    body = {
        "model": MODEL,
        "messages": [{"role": "user", "content": prompt}],
        "stream": True,
    }
    body.update(extra)

    req = urllib.request.Request(
        URL,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"},
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            raw = resp.read().decode("utf-8", "replace").strip()
    except Exception as e:
        return {"error": str(e)}

    if not raw.startswith("{"):
        return {"error": f"非 JSON: {raw[:200]}"}
    obj = json.loads(raw)
    if "error" in obj:
        return {"error": obj["error"]}
    ch = (obj.get("choices") or [{}])[0]
    msg = ch.get("message") or {}
    u = obj.get("usage") or {}
    det = u.get("completion_tokens_details") or {}
    return {
        "model": obj.get("model"),
        "content": (msg.get("content") or "")[:60],
        "reasoning_chars": len(msg.get("reasoning_content") or ""),
        "completion_tokens": u.get("completion_tokens"),
        "reasoning_tokens": det.get("reasoning_tokens"),
    }


if __name__ == "__main__":
    cases = [
        ("不传 effort", {}),
        ("low", {"reasoning_effort": "low"}),
        ("high", {"reasoning_effort": "high"}),
        ("max", {"reasoning_effort": "max"}),
    ]
    for name, extra in cases:
        r = call(extra)
        if "error" in r:
            print(f"{name:14} 失败: {r['error']}")
        else:
            print(
                f"{name:14} model={r['model']:24} "
                f"reasoning_tokens={r['reasoning_tokens']:>4} "
                f"completion={r['completion_tokens']:>4}  "
                f"{r['content']}"
            )
