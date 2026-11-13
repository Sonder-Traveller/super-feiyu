#!/usr/bin/env python3
"""删掉 config.toml 里残留的旧 chat 注释块（重复的能力标题 + 错误映射）。"""
from __future__ import annotations

from pathlib import Path

CFG = Path.home() / "super-feiyu/config.toml"
text = CFG.read_text(encoding="utf-8")

# 旧块：从第一个 "# ══════════ 能力 ══════════" 到第二个同样标题之前
marker = "# ══════════ 能力 ══════════"
first = text.find(marker)
second = text.find(marker, first + 1)

if first == -1 or second == -1:
    raise SystemExit("没找到重复的能力标题块，跳过")

removed = text[first:second]
text = text[:first] + text[second:]
CFG.write_text(text, encoding="utf-8")

print(f"删掉 {len(removed)} 字符的旧注释块")
print("--- 删掉的内容 ---")
print(removed.strip()[:400])
