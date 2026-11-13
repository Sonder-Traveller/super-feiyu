#!/usr/bin/env bash
# 把 Windows 侧改动同步到 WSL 并重建
set -euo pipefail
WIN=/mnt/c/Users/Assistant/projects/super-feiyu
DST="$HOME/super-feiyu"

cd "$DST"
rsync -a "$WIN/src/" src/
rsync -a "$WIN/tests/" tests/
rsync -a "$WIN/build.sh" .
rsync -a "$WIN/README.md" .
echo "同步完成"
./build_safe.sh 2>&1 | tail -12
