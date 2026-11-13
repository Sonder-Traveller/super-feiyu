#!/usr/bin/env python3
"""把 config.toml 里的 [capability.chat] 段替换成新内容（保留其余全部）。"""
from __future__ import annotations

import re
import shutil
from pathlib import Path

CFG = Path.home() / "super-feiyu/config.toml"
NEW = Path("/mnt/c/Users/Assistant/projects/super-feiyu/config_chat_section.txt")

text = CFG.read_text(encoding="utf-8")
new_section = NEW.read_text(encoding="utf-8").rstrip() + "\n"

# 从 [capability.chat] 到下一个 [section] 之前
pattern = re.compile(
    r"\[capability\.chat\][^\[]*",
    re.MULTILINE,
)
m = pattern.search(text)
if not m:
    raise SystemExit("没找到 [capability.chat] 段")

shutil.copy2(CFG, CFG.with_suffix(".toml.bak-chat"))

# 保留 [capability.chat] 之前的 "# ══════════ 能力 ══════════" 标题
before = text[: m.start()]
# 去掉 before 末尾已经重复的能力标题块
before = re.sub(r"# ══════════ 能力 ══════════\s*$", "", before)

updated = before + new_section + "\n" + text[m.end() :]
CFG.write_text(updated, encoding="utf-8")
print("已更新 [capability.chat]")
print("备份:", CFG.with_suffix(".toml.bak-chat"))
