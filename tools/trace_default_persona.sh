#!/usr/bin/env bash
# 追踪 AstrBot 默认人格文本的历史变化
set -u
cd /tmp/refs/AstrBotDevs_AstrBot || exit 1

echo "════════ 默认人格的定义位置"
grep -rn "DEFAULT_PERSONALITY" --include='*.py' . | head -5

echo
echo "════════ 该文件的历史版本里 prompt 的变化"
FILE=$(grep -rln "DEFAULT_PERSONALITY" --include='*.py' . | head -1)
echo "文件: $FILE"

for commit in $(git log --format=%h -5 -- "$FILE" 2>/dev/null); do
  echo "--- $commit  $(git log -1 --format='%s' $commit)"
  git show "$commit:$FILE" 2>/dev/null | grep -A4 "DEFAULT_PERSONALITY" | head -8
  echo
done
