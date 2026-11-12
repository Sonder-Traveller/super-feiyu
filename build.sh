#!/usr/bin/env bash
# 构建 super-feiyu（clang C++20 模块）。
# 用法: ./build.sh [--clean]
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SRC_DIR"

CXX="${CXX:-clang++}"
PREFIX="${PREFIX:-$HOME/local}"
OUT="${OUT:-build}"

INC=(-Iinclude -I"$PREFIX/include" -I"$PREFIX/include/x86_64-linux-gnu" -Isrc)
MODULES=(core log config memory capability transport orchestrator)

if [[ "${1:-}" == "--clean" ]]; then
  rm -rf "$OUT" build_obj build_pcm
fi

mkdir -p "$OUT/pcm" "$OUT/obj"

echo "==> 1/4 预编译模块接口"
FM=()
for m in "${MODULES[@]}"; do
  echo "    fish.$m"
  "$CXX" -std=c++20 "${INC[@]}" "${FM[@]}" \
    --precompile "src/fish.$m.cppm" -o "$OUT/pcm/fish.$m.pcm"
  FM+=("-fmodule-file=fish.$m=$OUT/pcm/fish.$m.pcm")
done

echo "==> 2/4 生成模块目标代码"
FM=()
for m in "${MODULES[@]}"; do
  "$CXX" -std=c++20 "${FM[@]}" -c "$OUT/pcm/fish.$m.pcm" -o "$OUT/obj/fish.$m.o"
  FM+=("-fmodule-file=fish.$m=$OUT/pcm/fish.$m.pcm")
done

echo "==> 3/4 编译主程序与测试"
"$CXX" -std=c++20 "${INC[@]}" "${FM[@]}" -c src/main.cpp -o "$OUT/obj/main.o"
"$CXX" -std=c++20 "${INC[@]}" "${FM[@]}" -c tests/test_all.cpp -o "$OUT/obj/test_all.o"
"$CXX" -std=c++20 "${INC[@]}" "${FM[@]}" -c tests/test_e2e.cpp -o "$OUT/obj/test_e2e.o"

echo "==> 4/4 链接"
LIBS=(-L"$PREFIX/lib" -lcurl -lsqlite3 -Wl,-rpath,"$PREFIX/lib")
"$CXX" "$OUT/obj/main.o" "$OUT"/obj/fish.*.o -o "$OUT/super-feiyu" "${LIBS[@]}"
"$CXX" "$OUT/obj/test_all.o" "$OUT"/obj/fish.*.o -o "$OUT/test_all" "${LIBS[@]}"
"$CXX" "$OUT/obj/test_e2e.o" "$OUT"/obj/fish.*.o -o "$OUT/test_e2e" "${LIBS[@]}"

echo
echo "构建完成:"
echo "  $OUT/super-feiyu"
echo "  $OUT/test_all"
echo "  $OUT/test_e2e"
