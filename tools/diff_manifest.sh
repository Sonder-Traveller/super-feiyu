#!/usr/bin/env bash
# 找出图片文件与 manifest 条目的差异
set -u
cd "$HOME/super-feiyu" || exit 1

D=data/memory/portraits
M=$D/manifest.md

echo "--- 有图片但 manifest 没记:"
for f in "$D"/*.png; do
  b=$(basename "$f")
  if ! grep -qF "$b" "$M"; then
    echo "  $b"
  fi
done

echo "--- manifest 记了但图片不存在:"
grep -v '^#' "$M" | grep -v '^$' | cut -d'|' -f1 | sed 's/^ *//;s/ *$//' | while read -r p; do
  [ -z "$p" ] && continue
  if [ ! -f "$p" ]; then
    echo "  $p"
  fi
done

echo "--- 计数:"
echo "  图片 $(find "$D" -maxdepth 1 -name '*.png' | wc -l) 张"
echo "  manifest $(grep -v '^#' "$M" | grep -vc '^$') 条"
