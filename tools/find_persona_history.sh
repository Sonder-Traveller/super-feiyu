#!/usr/bin/env bash
# 从 AstrBot 和 LangBot 的历史里找人格文本的演进
set -u
cd /tmp/refs || exit 1

echo "════════ AstrBot: persona 相关的历史提交"
cd AstrBotDevs_AstrBot || exit 1
# 拉完整历史（浅克隆只有 1 个提交）
timeout 300 git fetch --unshallow -q 2>/dev/null || echo "(无法拉完整历史)"
git log --oneline --all -- '*persona*' 2>/dev/null | head -20
