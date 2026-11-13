#!/usr/bin/env bash
# 看 7818a9c2 这次「保持单个可编辑的系统默认人格」到底改了什么人格文本
set -u
cd /tmp/refs/AstrBotDevs_AstrBot || exit 1

echo "=== 提交说明"
git log -1 --format='%s%n%n%b' 7818a9c2 | head -30

echo
echo "=== 该提交对 persona_mgr.py 的改动"
git show 7818a9c2 -- astrbot/core/persona_mgr.py | head -80
