#!/usr/bin/env bash
# 盘点候选素材仓库：授权 + 图片数量 + 体积
set -u
cd /tmp/asr || exit 1

for d in */; do
  n="${d%/}"
  echo "════════ $n"
  for f in LICENSE LICENSE.md LICENSE-CC-BY-NC-SA-4.0 SOURCES.md SOURCE.md; do
    if [ -f "$n/$f" ]; then
      echo "--- $f (前 12 行)"
      head -12 "$n/$f"
      echo
    fi
  done
  cnt=$(find "$n" -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.webp' -o -iname '*.gif' \) | wc -l)
  sz=$(du -sh "$n" | cut -f1)
  echo ">>> 图片 $cnt 张, 总体积 $sz"
  echo
done
