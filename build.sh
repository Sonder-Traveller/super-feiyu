#!/usr/bin/env bash
# 构建 super-feiyu（clang C++20 模块）。
# 用法: ./build.sh [--clean]
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SRC_DIR"

CXX="${CXX:-clang++}"
PREFIX="${PREFIX:-$HOME/local}"
OUT="${OUT:-build}"

# 模块 pcm 必须用**绝对路径**传给 -fmodule-file。
#
# 踩过的坑：用相对路径（build/pcm/fish.x.pcm）时，clang 21.1.8 会在
# 序列化模块 AST 阶段随机段错误（崩溃点有时 memory、有时 channels、
# 有时 orchestrator，与模块内容无关）。同一批命令改成绝对路径就全部正常，
# 加大 ulimit -s 也没用 —— 是编译器处理相对路径时的 bug，不是代码问题。
OUT_ABS="$(cd "$SRC_DIR" && mkdir -p "$OUT" && cd "$OUT" && pwd)"

INC=(-Iinclude -I"$PREFIX/include" -I"$PREFIX/include/x86_64-linux-gnu" -Isrc -Ithird_party/qrcodegen \
     -Ithird_party/thread-pool/pool/include)
# 编译顺序：依赖在前。
# persona / api 只依赖 core + log，所以排在 capability 之后、orchestrator 之前。
MODULES=(core log config memory capability probe media image meme persona control api session tool guard ws qq_ws onebot_ws channels dispatcher orchestrator)

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
  FM+=("-fmodule-file=fish.$m=$OUT_ABS/pcm/fish.$m.pcm")
done

echo "==> 2/4 生成模块目标代码"
FM=()
for m in "${MODULES[@]}"; do
  "$CXX" -std=c++20 "${FM[@]}" -c "$OUT/pcm/fish.$m.pcm" -o "$OUT/obj/fish.$m.o"
  FM+=("-fmodule-file=fish.$m=$OUT_ABS/pcm/fish.$m.pcm")
done

# 第三方普通 TU（非模块）：qrcodegen 是 MIT 的单文件实现，
# 不改成模块——改动它就得跟上游同步维护，得不偿失。
EXTRA_OBJS=()
for f in third_party/qrcodegen/qrcodegen.cpp; do
  obj="$OUT/obj/$(basename "${f%.cpp}").o"
  "$CXX" -std=c++20 -Ithird_party/qrcodegen -c "$f" -o "$obj"
  EXTRA_OBJS+=("$obj")
done

echo "==> 3/4 编译主程序与测试"
"$CXX" -std=c++20 "${INC[@]}" "${FM[@]}" -c src/main.cpp -o "$OUT/obj/main.o"

# 测试二进制统一由这个列表驱动：加测试只需往这里添名字。
# 之前是逐个 if [ -f ... ] 硬写，结果 test_memory 一直没被构建，
# 里面的代码停留在头文件时代（include 了早已不存在的 hpp）也没人发现。
TESTS=(test_all test_e2e test_protocol test_lifecycle test_memory)
COMPILED_TESTS=()
for t in "${TESTS[@]}"; do
  if [ -f "tests/$t.cpp" ]; then
    if "$CXX" -std=c++20 "${INC[@]}" "${FM[@]}" -c "tests/$t.cpp" -o "$OUT/obj/$t.o" 2>"$OUT/obj/$t.err"; then
      COMPILED_TESTS+=("$t")
    else
      echo "    ✗ $t 编译失败:"
      head -10 "$OUT/obj/$t.err" | sed 's/^/       /'
      exit 1
    fi
  fi
done
echo "    编译了 ${#COMPILED_TESTS[@]} 个测试: ${COMPILED_TESTS[*]}"

echo "==> 4/4 链接"
LIBS=(-L"$PREFIX/lib" -lcurl -lsqlite3 -lz -lcrypto -lpthread -Wl,-rpath,"$PREFIX/lib")
"$CXX" "$OUT/obj/main.o" "$OUT"/obj/fish.*.o "${EXTRA_OBJS[@]}" -o "$OUT/super-feiyu" "${LIBS[@]}"
for t in "${COMPILED_TESTS[@]}"; do
  "$CXX" "$OUT/obj/$t.o" "$OUT"/obj/fish.*.o "${EXTRA_OBJS[@]}" -o "$OUT/$t" "${LIBS[@]}"
done

echo
echo "构建完成:"
for b in super-feiyu "${COMPILED_TESTS[@]}"; do
  [ -x "$OUT/$b" ] && echo "  $OUT/$b  ($(du -h "$OUT/$b" | cut -f1))"
done
