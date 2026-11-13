#!/usr/bin/env python3
"""诊断提示词缺陷：统计追问率、工具误用、是否真干活。"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:18091/v1/chat/completions"
MODEL = "fast-model"

TOOLS = [
    {"type": "function", "function": {
        "name": "send_photo", "description": "发一张自己的照片给对方。返回图片。",
        "parameters": {"type": "object", "properties": {
            "scene": {"type": "string", "description": "画面内容"}}}}},
    {"type": "function", "function": {
        "name": "send_voice", "description": "把这句话用语音说出来，代替文字。",
        "parameters": {"type": "object", "properties": {
            "text": {"type": "string", "description": "要念出来的话。"}},
            "required": ["text"]}}},
    {"type": "function", "function": {
        "name": "search_memory", "description": "检索过往对话与事实。返回匹配的片段。",
        "parameters": {"type": "object", "properties": {
            "query": {"type": "string", "description": "要检索的内容。"}},
            "required": ["query"]}}},
]

# 单轮测试：这些场景应该「答完就停」，不该反问
SINGLE = [
    "1+1等于几",
    "Python 里怎么把两个 dict 合并",
    "今天股票涨了吗",
    "推荐一部电影",
    "我累了",
    "你好",
]

# 这些场景必须真干活，不能只说"贴代码给我看"
WORK = [
    "这段代码为什么崩：int* p = nullptr; *p = 1;",
    "帮我把 hello world 用 rust 写一遍",
]


def call(system: str, user: str, history: list | None = None) -> dict:
    messages = [{"role": "system", "content": system}]
    if history:
        messages.extend(history)
    messages.append({"role": "user", "content": user})

    body = {"model": MODEL, "messages": messages, "reasoning_effort": "high",
            "stream": True, "tools": TOOLS, "tool_choice": "auto"}
    req = urllib.request.Request(
        URL, data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer bridge-local"})
    with urllib.request.urlopen(req, timeout=180) as resp:
        raw = resp.read().decode("utf-8", "replace").strip()
    obj = json.loads(raw)
    ch = (obj.get("choices") or [{}])[0]
    msg = ch.get("message") or {}
    return {"content": (msg.get("content") or "").strip(),
            "tool_calls": msg.get("tool_calls") or []}


def ends_with_question(text: str) -> bool:
    t = text.strip().rstrip("。…「」\"'")
    return t.endswith("？") or t.endswith("?") or "呢？" in text[-15:]


def main() -> int:
    system = Path(sys.argv[1]).read_text(encoding="utf-8")
    print(f"=== {Path(sys.argv[1]).name} ({len(system)} 字符)\n")

    print("── 单轮：应当答完就停")
    q = 0
    for user in SINGLE:
        r = call(system, user)
        asked = ends_with_question(r["content"])
        q += asked
        mark = "追问" if asked else "OK"
        tools = [t.get("function", {}).get("name") for t in r["tool_calls"]]
        print(f"[{mark}] {user}")
        print(f"       {r['content'][:60]!r}")
        if tools:
            print(f"       工具: {tools}")
    print(f"\n追问率: {q}/{len(SINGLE)}\n")

    print("── 工作：必须直接给答案")
    for user in WORK:
        r = call(system, user)
        print(f"· {user}")
        print(f"  {r['content'][:200]!r}")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
