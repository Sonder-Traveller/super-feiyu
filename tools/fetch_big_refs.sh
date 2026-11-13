#!/usr/bin/env bash
# 拉取高 star 的参考项目（AstrBot 46k / LangBot 19k / SillyTavern）
set -u
cd /tmp || exit 1
rm -rf refs
mkdir -p refs
cd refs || exit 1

clone() {
  local r="$1"
  local n="${r//\//_}"
  if timeout 300 git clone --depth 1 -q "https://github.com/$r.git" "$n" 2>/dev/null; then
    echo "OK   $r"
  else
    echo "FAIL $r"
  fi
}

clone AstrBotDevs/AstrBot
clone RockChinQ/LangBot

echo
echo "=== 默认人格/提示词文件:"
find . -type f \( -name '*.md' -o -name '*.txt' -o -name '*.json' -o -name '*.yaml' \) \
  | grep -iE 'persona|personality|prompt|system' | head -30
