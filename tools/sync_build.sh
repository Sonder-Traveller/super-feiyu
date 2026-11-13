#!/usr/bin/env bash
# 同步源码到 WSL 部署目录并构建（只同步代码，不动 data）
set -euo pipefail

WIN=/mnt/c/Users/Assistant/projects/super-feiyu
DST="$HOME/super-feiyu"

cd "$DST"

# 同步 src / persona / 构建脚本 / 配置
rsync -a --delete "$WIN/src/" src/
rsync -a "$WIN/build.sh" .
rsync -a "$WIN/CMakeLists.txt" .
rsync -a "$WIN/config.toml" .

# persona 目录：只保留用户的默认人格，不覆盖已有内容
mkdir -p persona
[ -f persona/persona.md ] || : > persona/persona.md

echo "--- persona.md: $(stat -c%s persona/persona.md) 字节"
echo "--- 模块数: $(grep -c 'fish\..*\.cppm' CMakeLists.txt)"
echo "--- 构建:"
./build.sh 2>&1 | tail -12
