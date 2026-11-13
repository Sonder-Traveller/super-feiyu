#!/usr/bin/env bash
# 拉取参考项目，用于研究它们的人格提示词写法。
set -u
cd /tmp || exit 1
rm -rf refs
mkdir -p refs
cd refs || exit 1

REPOS=(
  whyever77/dafeiyu-astrbot
  BurninIce/nurture-a-life
  LanternFlower/galgame-astrbot-personas
)

for r in "${REPOS[@]}"; do
  n="${r//\//_}"
  if timeout 180 git clone --depth 1 -q "https://github.com/$r.git" "$n" 2>/dev/null; then
    echo "OK   $r"
  else
    echo "FAIL $r"
  fi
done

echo "--- 找到的人格文件:"
find . -type f \( -iname '*persona*' -o -iname '*人格*' -o -iname '*character*' \) \
  -name '*.md' | head -20
