#!/usr/bin/env python3
"""用真实 API 试系统提示词。

用法:
    python3 tools/try_prompt.py <提示词文件> [场景名...]

每个场景发一条用户消息，打印模型回复。
不传场景名就跑全部场景。
"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:18091/v1/chat/completions"
MODEL = "fast-model"

# 覆盖各种日常场景：闲聊、被夸、被问正经事、被要图、问时间、问记忆
SCENES: dict[str, str] = {
    "闲聊": "在干嘛呢",
    "被夸": "你今天好看",
    "叫胖": "你是不是又胖了",
    "正经": "python 里 list 和 tuple 有什么区别",
    "要图": "发张照片看看",
    "问时间": "现在几点了",
    "问记忆": "还记得我们上次聊什么吗",
    "长问题": "帮我分析一下为什么我的 C++ 程序编译很慢，用的是 clang 和模块",
    "梗": "你知道「吃白饭」这个梗吗",
    "不想聊": "算了不想说了",
}


def call(system: str, user: str, tools: list | None = None) -> dict:
    messages = []
    if system:
        messages.append({"role": "system", "content": system})
    messages.append({"role": "user", "content": user})

    body: dict = {
        "model": MODEL,
        "messages": messages,
        "reasoning_effort": "high",
        "stream": True,
    }
    if tools:
        body["tools"] = tools
        body["tool_choice"] = "auto"

    req = urllib.request.Request(
        URL,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "Authorization": "Bearer bridge-local"},
    )
    with urllib.request.urlopen(req, timeout=180) as resp:
        raw = resp.read().decode("utf-8", "replace").strip()

    obj = json.loads(raw)
    if "error" in obj:
        return {"error": obj["error"]}
    ch = (obj.get("choices") or [{}])[0]
    msg = ch.get("message") or {}
    return {
        "content": (msg.get("content") or "").strip(),
        "tool_calls": msg.get("tool_calls") or [],
        "model": obj.get("model"),
    }


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: try_prompt.py <提示词文件> [场景名...]")
        return 2

    prompt_file = Path(sys.argv[1])
    if not prompt_file.exists():
        print(f"文件不存在: {prompt_file}")
        return 2
    system = prompt_file.read_text(encoding="utf-8")

    wanted = sys.argv[2:] or list(SCENES)
    print(f"=== 提示词: {prompt_file}  ({len(system)} 字符)")
    print(f"=== 模型: {MODEL}\n")

    for name in wanted:
        user = SCENES.get(name, name)
        try:
            r = call(system, user)
        except Exception as e:
            print(f"【{name}】失败: {e}\n")
            continue
        if "error" in r:
            print(f"【{name}】错误: {r['error']}\n")
            continue
        print(f"【{name}】用户: {user}")
        print(f"  模型: {r['content'] or '(空)'}")
        if r["tool_calls"]:
            for tc in r["tool_calls"]:
                fn = tc.get("function", {})
                print(f"  [工具] {fn.get('name')} {fn.get('arguments')}")
        print()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
