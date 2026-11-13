#!/usr/bin/env bash
# 找 AstrBot 内置的默认人格提示词
set -u
cd /tmp/refs/AstrBotDevs_AstrBot || exit 1

echo "=== 搜 persona 相关默认值:"
grep -rn "你叫\|你是一个\|你是「" --include='*.py' --include='*.json' --include='*.md' . \
  | grep -viE 'test|i18n|locale|dashboard' | head -20

echo
echo "=== persona 相关 py 文件:"
find . -name '*persona*' -type f | head -20
