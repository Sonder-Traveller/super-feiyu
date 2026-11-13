#!/usr/bin/env python3
"""验证 fast-model + reasoning_effort=high 的组合效果。"""
from __future__ import annotations

import json
import urllib.request

URL = "http://127.0.0.1:18091/v1/chat/completions"


def call(model: str, effort: str | None, prompt: str) -> dict:
    body: dict = {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "stream": True,
    }
    if effort:
        body["reasoning_effort"] = effort

    req = urllib.request.Request(
        URL,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"},
    )
    try:
        with urllib.request.urlopen(req, timeout=180) as resp:
            raw = resp.read().decode("utf-8", "replace").strip()
    except Exception as e:
        return {"error": str(e)}

    if not raw.startswith("{"):
        return {"error": raw[:200]}
    obj = json.loads(raw)
    if "error" in obj:
        return {"error": obj["error"]}
    msg = (obj.get("choices") or [{}])[0].get("message") or {}
    u = obj.get("usage") or {}
    det = u.get("completion_tokens_details") or {}
    return {
        "model": obj.get("model"),
        "content": (msg.get("content") or "").strip(),
        "reasoning_tokens": det.get("reasoning_tokens"),
        "completion": u.get("completion_tokens"),
    }


PROMPT = "一个农夫有17只羊，除了9只以外都死了，还剩几只？"

if __name__ == "__main__":
    for model in ("fast-model", "deep-model"):
        for effort in (None, "high"):
            r = call(model, effort, PROMPT)
            tag = f"{model} effort={effort or '默认'}"
            if "error" in r:
                print(f"{tag:34} 失败: {r['error']}")
                continue
            print(f"{tag:34} model={r['model']:22} "
                  f"think={r['reasoning_tokens']:>4} tok={r['completion']:>4}")
            print(f"{'':34} 答: {r['content'][:70]}")
