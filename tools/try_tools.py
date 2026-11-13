#!/usr/bin/env python3
"""用真实 API 试系统提示词 + 工具定义（含工具结果回填，模拟真实流程）。

发图/发语音做成工具调用，不用文本标记。
"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:18091/v1/chat/completions"
MODEL = "fast-model"

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "send_photo",
            "description": "发一张自己的照片给对方。返回图片。",
            "parameters": {
                "type": "object",
                "properties": {
                    "scene": {
                        "type": "string",
                        "description": "画面内容，如「在吃泡面」「趴在桌上睡着了」。",
                    }
                },
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "send_voice",
            "description": "把这句话用语音说出来，代替文字。",
            "parameters": {
                "type": "object",
                "properties": {
                    "text": {"type": "string", "description": "要念出来的话。"}
                },
                "required": ["text"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "search_memory",
            "description": "检索过往对话与事实。返回匹配的片段。",
            "parameters": {
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "要检索的内容。"}
                },
                "required": ["query"],
            },
        },
    },
]

SCENES: dict[str, str] = {
    "闲聊": "在干嘛呢",
    "正经": "python 里 list 和 tuple 有什么区别",
    "要图": "发张照片看看",
    "要图2": "你在干嘛，拍张看看",
    "语音": "用语音说一句晚安",
    "不该发图": "你今天好看",
    "问记忆": "还记得我们上次聊什么吗",
}


def raw_call(messages: list, tools: list | None) -> dict:
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
    }


def fake_tool_result(name: str, args: dict) -> str:
    if name == "send_photo":
        scene = args.get("scene") or "随机"
        return f"[已发送图片，场景：{scene}]"
    if name == "send_voice":
        return f"[已发送语音：{args.get('text', '')}]"
    if name == "search_memory":
        return "[记忆里没有相关内容]"
    return "[ok]"


def run_scene(system: str, user: str, max_rounds: int = 3) -> None:
    messages = [
        {"role": "system", "content": system},
        {"role": "user", "content": user},
    ]
    tool_log: list[str] = []

    for _ in range(max_rounds):
        r = raw_call(messages, TOOLS)
        if "error" in r:
            print(f"  错误: {r['error']}")
            return

        if r["tool_calls"]:
            # 记录并回填工具结果，模拟真实流程
            messages.append(
                {"role": "assistant", "content": r["content"] or "", "tool_calls": r["tool_calls"]}
            )
            for tc in r["tool_calls"]:
                fn = tc.get("function", {})
                name = fn.get("name", "?")
                try:
                    args = json.loads(fn.get("arguments") or "{}")
                except json.JSONDecodeError:
                    args = {}
                tool_log.append(f"{name}({json.dumps(args, ensure_ascii=False)})")
                result = fake_tool_result(name, args)
                messages.append(
                    {
                        "role": "tool",
                        "tool_call_id": tc.get("id", ""),
                        "content": result,
                    }
                )
            continue

        # 没有工具调用 → 最终回复
        for t in tool_log:
            print(f"  → {t}")
        print(f"  说: {r['content'] or '(空)'}")
        return

    for t in tool_log:
        print(f"  → {t}")
    print("  (达到轮数上限)")


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: try_tools.py <提示词文件> [场景名...]")
        return 2

    prompt_file = Path(sys.argv[1])
    system = prompt_file.read_text(encoding="utf-8")

    wanted = sys.argv[2:] or list(SCENES)
    print(f"=== 提示词: {prompt_file.name}  ({len(system)} 字符)\n")

    for name in wanted:
        user = SCENES.get(name, name)
        print(f"【{name}】{user}")
        try:
            run_scene(system, user)
        except Exception as e:
            print(f"  失败: {e}")
        print()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
