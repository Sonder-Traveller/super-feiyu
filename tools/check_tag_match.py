#!/usr/bin/env python3
"""复现形象库标签匹配命中率。

find_by_tag 用的是双向子串匹配（t.find(q) 或 q.find(t)），
模型传进来的 query 是自然语言短语，标签是短词。
如果命中率低，模型就会说「没有图片可发」。
"""
import sys

rows = []
for line in open("/home/assistant/super-feiyu/data/memory/portraits/manifest.md",
                 encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    parts = [p.strip() for p in line.split("|")]
    if len(parts) >= 3:
        rows.append([t for t in parts[2].split(",") if t])

print(f"形象库条目: {len(rows)}")


def find(tag):
    out = []
    for tags in rows:
        for t in tags:
            if tag in t or t in tag:
                out.append(tags)
                break
    return out


queries = [
    "吃饭", "照片", "自拍", "你好", "生气", "开心", "日常", "鲸鱼",
    "发张照片", "发个你的照片看看", "吃饭的照片", "生气的样子",
    "可爱的自拍", "我在吃饭", "看看你", "你的样子",
]
low = 0
for q in queries:
    n = len(find(q))
    if n == 0:
        low += 1
    mark = "  <-- 0 命中" if n == 0 else ""
    print(f"{q!r:22} -> {n:3} hits{mark}")

print(f"\n零命中: {low}/{len(queries)}")
