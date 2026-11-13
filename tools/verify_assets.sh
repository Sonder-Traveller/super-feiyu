#!/usr/bin/env bash
# 校验导入结果：数量、体积、超限检查
set -u
cd "$HOME/super-feiyu" || exit 1

for d in data/memory/portraits data/memory/portraits-local; do
  echo "════════ $d"
  n=$(find "$d" -maxdepth 1 -name '*.png' | wc -l)
  sz=$(du -sh "$d" | cut -f1)
  echo "图片 $n 张, 总计 $sz"
  echo "--- 最大的 5 张:"
  find "$d" -maxdepth 1 -name '*.png' -printf '%s\t%f\n' | sort -rn | head -5 | \
    awk -F'\t' '{printf "  %6d KB  %s\n", $1/1024, $2}'
  echo "--- 超过 700KB 的（QQ base64 后可能超 5MB）:"
  find "$d" -maxdepth 1 -name '*.png' -size +700k -printf '  %s\t%p\n' | head
  echo
done

echo "════════ manifest 行数"
grep -vc '^#' data/memory/portraits/manifest.md
