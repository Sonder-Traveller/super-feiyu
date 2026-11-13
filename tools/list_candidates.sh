#!/usr/bin/env bash
# 列出候选入库文件（授权清晰的三个仓库）
set -u
cd /tmp/asr || exit 1

echo "════════ Neko3000 / character-samples (cn)"
ls Neko3000_deepseek-whalechan/assets/readme/cn/character-samples/

echo
echo "════════ Neko3000 / comic-references"
ls Neko3000_deepseek-whalechan/skills/whalechan-image-comic/assets/comic-references/

echo
echo "════════ Neko3000 / 主目录 assets"
find Neko3000_deepseek-whalechan/assets -maxdepth 2 -type f \( -iname '*.webp' -o -iname '*.png' -o -iname '*.jpg' \) -printf '%p\n' | head -20

echo
echo "════════ 1190fasheqi sprites"
ls -la 1190fasheqi_dafeiyu-pet/sprites/

echo
echo "════════ bloodarea"
find bloodarea_dsh-pets/assets -type f -printf '%s\t%p\n' | sort -rn
