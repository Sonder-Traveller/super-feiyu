#!/usr/bin/env bash
# 拉取候选素材仓库，供人工核对授权后筛选。
set -u

cd /tmp || exit 1
rm -rf asr
mkdir -p asr
cd asr || exit 1

REPOS=(
  bloodarea/dsh-pets
  YunYueSama/codex-deepseek-pet
  1190fasheqi/dafeiyu-pet
  gmskywalker/deepseek-fat-fish-codex-pet
  roseion/dsh-dayu-fish-skin
  Neko3000/deepseek-whalechan
)

for r in "${REPOS[@]}"; do
  n="${r//\//_}"
  if timeout 180 git clone --depth 1 -q "https://github.com/$r.git" "$n" 2>/dev/null; then
    echo "OK   $r"
  else
    echo "FAIL $r"
  fi
done

echo "--- sizes ---"
du -sh ./* 2>/dev/null
echo "--- license files ---"
find . -maxdepth 3 -iname 'LICENSE*' -o -maxdepth 3 -iname 'SOURCES*' -o -maxdepth 3 -iname 'SOURCE.md' 2>/dev/null
