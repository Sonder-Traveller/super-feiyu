#!/usr/bin/env bash
# 构建 super-feiyu（clang C++20 模块）。
# 用法: ./build.sh [--clean]
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SRC_DIR"

CXX="${CXX:-clang++}"
PREFIX="${PREFIX:-$HOME/local}"
OUT="${OUT:-build}"

INC=(-Iinclude -I"$PREFIX/include" -I"$PREFIX/include/x86_64-linux-gnu" -Isrc \
     -Ithird_party/thread-pool/pool/include)
# 编译顺序：依赖在前（media 依赖 capability，session 只依赖 core）
# 编译顺序：依赖在前（persona/api 只依赖 core/log）
MODULES=(core log config memory capability probe media image meme persona api session tool guard ws qq_ws onebot_ws channels dispatcher orchestrator)

if [[ "${1:-}" == "--clean" ]]; then
  rm -rf "$OUT"
fi

mkdir -p "$OUT/pcm" "$OUT/obj"

echo "==> 1/4 预编译模块接口 (${#MODULES[@]} 个)"
FM=()
for m in "${MODULES[@]}"; do
  printf "    fish.%-14s" "$m"
  if "$CXX" -std=c++20 "${INC[@]}" "${FM[@]}" \
      --precompile "src/fish.$m.cppm" -o "$OUT/pcm/fish.$m.pcm" 2>"$OUT/pcm/fish.$m.err"; then
    echo "ok"
  else
    echo "FAILED"
    head -20 "$OUT/pcm/fish.$m.err"
    exit 1
  fi
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
if [ -f tests/test_protocol.cpp ]; then
  "$CXX" -std=c++20 "${INC[@]}" "${FM[@]}" -c tests/test_protocol.cpp -o "$OUT/obj/test_protocol.o"
fi
if [ -f tests/test_lifecycle.cpp ]; then
  "$CXX" -std=c++20 "${INC[@]}" "${FM[@]}" -c tests/test_lifecycle.cpp -o "$OUT/obj/test_lifecycle.o"
fi

echo "==> 4/4 链接"
LIBS=(-L"$PREFIX/lib" -lcurl -lsqlite3 -lpthread -Wl,-rpath,"$PREFIX/lib")
"$CXX" "$OUT/obj/main.o" "$OUT"/obj/fish.*.o -o "$OUT/super-feiyu" "${LIBS[@]}"
"$CXX" "$OUT/obj/test_all.o" "$OUT"/obj/fish.*.o -o "$OUT/test_all" "${LIBS[@]}"
"$CXX" "$OUT/obj/test_e2e.o" "$OUT"/obj/fish.*.o -o "$OUT/test_e2e" "${LIBS[@]}"
if [ -f "$OUT/obj/test_protocol.o" ]; then
  "$CXX" "$OUT/obj/test_protocol.o" "$OUT"/obj/fish.*.o -o "$OUT/test_protocol" "${LIBS[@]}"
fi
if [ -f "$OUT/obj/test_lifecycle.o" ]; then
  "$CXX" "$OUT/obj/test_lifecycle.o" "$OUT"/obj/fish.*.o -o "$OUT/test_lifecycle" "${LIBS[@]}"
fi

echo
echo "构建完成:"
for b in super-feiyu test_all test_e2e test_protocol test_lifecycle; do
  [ -x "$OUT/$b" ] && echo "  $OUT/$b  ($(du -h "$OUT/$b" | cut -f1))"
done
