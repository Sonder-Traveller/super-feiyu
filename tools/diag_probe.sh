#!/usr/bin/env bash
# 诊断 fish.probe 编译失败
set -u
cd "$HOME/super-feiyu" || exit 1

echo "HOME=$HOME"
echo "PREFIX=${PREFIX:-未设置}"
echo "本地前缀存在: $([ -d "$HOME/local/include/nlohmann" ] && echo yes || echo no)"
echo "json.hpp: $([ -f "$HOME/local/include/nlohmann/json.hpp" ] && echo present || echo missing)"

echo
echo "=== build.sh 里的 INC 定义"
sed -n '9,20p' build.sh

echo
echo "=== 手动按 build.sh 的 INC 编译 probe"
INC=(-Iinclude -I"$HOME/local/include" -I"$HOME/local/include/x86_64-linux-gnu" -Isrc)
echo "INC: ${INC[*]}"
clang++ -std=c++20 "${INC[@]}" \
  -fmodule-file=fish.core=build/pcm/fish.core.pcm \
  -fmodule-file=fish.log=build/pcm/fish.log.pcm \
  --precompile src/fish.probe.cppm -o /tmp/p.pcm 2>&1 | head -8
echo "退出码: $?"

echo
echo "=== 加 -v 看实际搜索路径"
clang++ -std=c++20 "${INC[@]}" -E -v -x c++ /dev/null 2>&1 | grep -A8 'search starts here' | head -12
