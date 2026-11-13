#!/usr/bin/env bash
# 列出各仓库图片分布（按体积降序，只看前 15 张）
set -u
cd /tmp/asr || exit 1

for d in Neko3000_deepseek-whalechan roseion_dsh-dayu-fish-skin \
         1190fasheqi_dafeiyu-pet gmskywalker_deepseek-fat-fish-codex-pet \
         bloodarea_dsh-pets; do
  echo "════════ $d"
  find "$d" -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.webp' -o -iname '*.gif' \) \
    -printf '%s\t%p\n' | sort -rn | head -15
  echo "--- 目录分布:"
  find "$d" -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.webp' -o -iname '*.gif' \) \
    -printf '%h\n' | sort | uniq -c | sort -rn | head -10
  echo
done
