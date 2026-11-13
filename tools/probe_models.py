#!/usr/bin/env python3
"""探测桥接层接受哪些模型名，确认能否精确指定 deepseek。"""
from __future__ import annotations

import json
import urllib.request

URL = "http://127.0.0.1:18091/v1/chat/completions"

CANDIDATES = [
    "deepseek-v4.1-flash",
    "deepseek-v4.1-flash-sg",
    "deepseek-v4.1",
    "deepseek-flash",
    "deep-model",
    "balanced-model",
    "fast-model",
    "auto",
]


def probe(model: str) -> tuple[bool, str, str]:
    """返回 (是否可用, 实际模型, 说明)。"""
    body = json.dumps(
        {
            "model": model,
            "messages": [{"role": "user", "content": "只回答两个字：好的"}],
            "stream": True,
        }
    ).encode()

    req = urllib.request.Request(
        URL,
        data=body,
        headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"},
    )
    try:
        with urllib.request.urlopen(req, timeout=90) as resp:
            raw = resp.read().decode("utf-8", "replace").strip()
    except Exception as e:
        return False, "-", f"HTTP 异常: {e}"

    if not raw.startswith("{"):
        return False, "-", f"非 JSON: {raw[:120]}"
    try:
        obj = json.loads(raw)
    except json.JSONDecodeError:
        return False, "-", f"解析失败: {raw[:120]}"

    if "error" in obj:
        err = obj["error"]
        msg = err.get("message") if isinstance(err, dict) else str(err)
        return False, "-", f"上游错误: {msg}"

    used = obj.get("model", "?")
    ch = (obj.get("choices") or [{}])[0]
    content = (ch.get("message") or {}).get("content") or ""
    return True, used, content[:40]


if __name__ == "__main__":
    for m in CANDIDATES:
        ok, used, note = probe(m)
        flag = "OK  " if ok else "FAIL"
        print(f"{flag} {m:28} -> {used:24} {note}")
