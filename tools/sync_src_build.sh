#!/usr/bin/env bash
# 同步改动的源码并构建
set -euo pipefail
WIN=/mnt/c/Users/Assistant/projects/super-feiyu
DST="$HOME/super-feiyu"
cd "$DST"
rsync -a "$WIN/src/" src/
echo "同步完成"
./build.sh 2>&1 | tail -12
