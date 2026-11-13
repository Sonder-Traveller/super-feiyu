#!/usr/bin/env bash
# 定位：相对路径 vs 绝对路径 的 -fmodule-file 是否导致崩溃
set -u
cd "$HOME/super-feiyu" || exit 1

ulimit -s 65536 2>/dev/null || true

INC=(-Iinclude -I"$HOME/local/include" -I"$HOME/local/include/x86_64-linux-gnu" -Isrc \
     -Ithird_party/thread-pool/pool/include)
OUT=build
MODULES=(core log config memory capability probe media image meme persona api session tool guard ws qq_ws onebot_ws channels)

echo "=== A. 用相对路径（build_safe.sh 的写法）"
FM=()
for m in "${MODULES[@]}"; do
  FM+=("-fmodule-file=fish.$m=$OUT/pcm/fish.$m.pcm")
done
echo "    参数示例: ${FM[0]}"
clang++ -std=c++20 "${INC[@]}" "${FM[@]}" \
  --precompile src/fish.orchestrator.cppm -o /tmp/rel.pcm > /tmp/rel.log 2>&1
echo "    退出码: $?"
tail -2 /tmp/rel.log | sed 's/^/    /'

echo
echo "=== B. 用绝对路径"
FM=()
for m in "${MODULES[@]}"; do
  FM+=("-fmodule-file=fish.$m=$HOME/super-feiyu/$OUT/pcm/fish.$m.pcm")
done
echo "    参数示例: ${FM[0]}"
clang++ -std=c++20 "${INC[@]}" "${FM[@]}" \
  --precompile src/fish.orchestrator.cppm -o /tmp/abs.pcm > /tmp/abs.log 2>&1
echo "    退出码: $?"
tail -2 /tmp/abs.log | sed 's/^/    /'

echo
echo "=== C. 相对路径 + 加大栈"
ulimit -s 262144
clang++ -std=c++20 "${INC[@]}" \
  $(for m in "${MODULES[@]}"; do echo "-fmodule-file=fish.$m=$OUT/pcm/fish.$m.pcm"; done) \
  --precompile src/fish.orchestrator.cppm -o /tmp/rel2.pcm > /tmp/rel2.log 2>&1
echo "    退出码: $?"
tail -2 /tmp/rel2.log | sed 's/^/    /'
