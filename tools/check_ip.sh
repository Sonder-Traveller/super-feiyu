#!/usr/bin/env bash
# 看 Neko3000 的 IP/署名章节，确认可再分发范围
set -u
cd /tmp/asr/Neko3000_deepseek-whalechan || exit 1
awk '/Fanwork Copyright Attribution/,0' README.md | head -60
