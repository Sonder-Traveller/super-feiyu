#!/usr/bin/env bash
# 部署 super-feiyu 到 ~/super-feiyu（可运行形态）。
# 用法: ./deploy.sh
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${DEST:-$HOME/super-feiyu}"

echo "==> 部署到 $DEST"
mkdir -p "$DEST"

# 源码与构建脚本（排除构建产物，但保留脚本本身）
rsync -a --delete \
  --exclude '.git/' \
  --exclude 'build/' --exclude 'build_obj/' --exclude 'build_pcm/' \
  --exclude '*.o' --exclude '*.pcm' --exclude 'data/' \
  "$SRC_DIR"/ "$DEST"/

cd "$DEST"
chmod +x build.sh deploy.sh 2>/dev/null || true

echo "==> 构建"
./build.sh 2>&1 | tail -6

echo "==> 自检"
LD_LIBRARY_PATH="$HOME/local/lib" ./build/super-feiyu --self-test 2>&1 | tail -8

echo
echo "部署完成。"
echo "  可执行: $DEST/build/super-feiyu"
echo "  自检:   LD_LIBRARY_PATH=\$HOME/local/lib $DEST/build/super-feiyu --self-test"
echo "  运行:   LD_LIBRARY_PATH=\$HOME/local/lib $DEST/build/super-feiyu --config config.toml"
