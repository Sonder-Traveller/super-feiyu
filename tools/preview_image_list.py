#!/usr/bin/env python3
"""预览 send_image 返回给模型的清单长什么样。"""
import sys

path = "/home/assistant/super-feiyu/data/memory/portraits/manifest.md"
rows = []
for line in open(path, encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    parts = [p.strip() for p in line.split("|")]
    if len(parts) >= 2:
        rows.append(parts[1])

print(f"形象库共 {len(rows)} 张，清单：")
for i, cap in enumerate(rows[:8]):
    print(f"[{i}] {cap}")
if len(rows) > 8:
    print(f"...还有 {len(rows) - 8} 张（序号 8 起）")
print("选一张最合适的，带 index 参数再调用一次 send_image。")
