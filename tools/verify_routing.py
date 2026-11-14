#!/usr/bin/env python3
"""验证桥接层的模型路由与思考强度是否按配置生效。"""
from __future__ import annotations

import json
import urllib.request

URL = "http://127.0.0.1:18091/v1/chat/completions"


def call(model: str, effort: str | None) -> dict:
    body: dict = {
        "model": model,
        "messages": [{"role": "user", "content": "9.11 和 9.9 哪个大？"}],
        "stream": True,
    }
    if effort:
        body["reasoning_effort"] = effort

    req = urllib.request.Request(
        URL,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer bridge-local"},
    )
    with urllib.request.urlopen(req, timeout=180) as resp:
        raw = resp.read().decode("utf-8", "replace").strip()
    obj = json.loads(raw)
    u = obj.get("usage") or {}
    det = u.get("completion_tokens_details") or {}
    msg = (obj.get("choices") or [{}])[0].get("message") or {}
    return {
        "model": obj.get("model"),
        "think": det.get("reasoning_tokens"),
        "answer": (msg.get("content") or "").strip()[:60],
    }


if __name__ == "__main__":
    print("=== 配置验证（fast-model + high）")
    r = call("fast-model", "high")
    print(f"  实际模型: {r['model']}")
    print(f"  思考token: {r['think']}")
    print(f"  回答: {r['answer']}")
    print()
    print("=== 对照：不传 effort")
    r2 = call("fast-model", None)
    print(f"  实际模型: {r2['model']}")
    print(f"  思考token: {r2['think']}")
