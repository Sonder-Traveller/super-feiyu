#!/usr/bin/env bash
# 二分定位：orchestrator 崩溃是「模块自身」还是「累积了太多 -fmodule-file」
set -u
cd "$HOME/super-feiyu" || exit 1

INC=(-Iinclude -I"$HOME/local/include" -I"$HOME/local/include/x86_64-linux-gnu" -Isrc \
     -Ithird_party/thread-pool/pool/include)
PCM=/tmp/bisect_pcm
[ -d "$PCM" ] || { echo "缺少 $PCM，先跑 diag_seq.sh"; exit 1; }

echo "=== 测 1: orchestrator 只带它直接依赖的模块"
# orchestrator 的 import（看源码）
grep -E '^import fish\.' src/fish.orchestrator.cppm | sed 's/^/    /'

DIRECT=$(grep -oE '^import fish\.[a-z_]+;' src/fish.orchestrator.cppm | sed 's/import //;s/;//' | tr '\n' ' ')
echo "    直接依赖: $DIRECT"

FML=()
for m in $DIRECT; do
  [ -f "$PCM/fish.$m.pcm" ] && FML+=("-fmodule-file=fish.$m=$PCM/fish.$m.pcm")
done
echo "    参数数: ${#FML[@]}"
clang++ -std=c++20 "${INC[@]}" "${FML[@]}" --precompile src/fish.orchestrator.cppm -o /tmp/o1.pcm 2>&1 | tail -3
echo "    退出码: $?"

echo
echo "=== 测 2: orchestrator 带全部 19 个前序模块"
ALL=()
for f in "$PCM"/*.pcm; do
  n=$(basename "$f" .pcm)
  [ "$n" != "fish.orchestrator" ] && ALL+=("-fmodule-file=$n=$f")
done
echo "    参数数: ${#ALL[@]}"
clang++ -std=c++20 "${INC[@]}" "${ALL[@]}" --precompile src/fish.orchestrator.cppm -o /tmp/o2.pcm 2>&1 | tail -3
echo "    退出码: $?"

echo
echo "=== 测 3: 完全不带 -fmodule-file（应该报 module not found）"
clang++ -std=c++20 "${INC[@]}" --precompile src/fish.orchestrator.cppm -o /tmp/o3.pcm 2>&1 | head -3
