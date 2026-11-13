#!/usr/bin/env bash
# 绕过 build.sh 的预编译循环：把逐模块编译好的 pcm 拷进 build/，然后只跑后半段
#
# 背景：build.sh 的预编译循环在 --clean 之后偶发 clang 段错误
# （ASTWriter 序列化 std::hash<optional> 时栈溢出），
# 但同样的命令逐条单独跑就全部通过——疑似同一进程内累积状态导致。
# 这是 clang 21.1.8 的问题，不是代码问题。
set -euo pipefail
cd "$HOME/super-feiyu" || exit 1

# clang 21.1.8 在序列化模块 AST 时会栈溢出：
#   clang::StackExhaustionHandler::runWithSufficientStackSpace
#   clang::ASTWriter::WriteSubStmt  → 段错误
# 崩溃点是随机的（有时 memory，有时 channels），说明与模块内容无关，
# 是编译器自身的递归深度问题。默认栈只有 8MB，放大到 64MB 就稳定了。
ulimit -s 65536 2>/dev/null || true

PREFIX="$HOME/local"
INC=(-Iinclude -I"$PREFIX/include" -I"$PREFIX/include/x86_64-linux-gnu" -Isrc \
     -Ithird_party/thread-pool/pool/include)
MODULES=(core log config memory capability probe media image meme persona api session tool guard ws qq_ws onebot_ws channels dispatcher orchestrator)

OUT=build
mkdir -p "$OUT/pcm" "$OUT/obj"

echo "==> 1/4 预编译（每模块独立进程，失败即止）"
FM=()
for m in "${MODULES[@]}"; do
  printf "    fish.%-14s" "$m"
  if ! clang++ -std=c++20 "${INC[@]}" "${FM[@]}" \
      --precompile "src/fish.$m.cppm" -o "$OUT/pcm/fish.$m.pcm" \
      > "$OUT/pcm/fish.$m.err" 2>&1; then
    echo "FAILED"
    tail -5 "$OUT/pcm/fish.$m.err" | sed 's/^/       /'
    exit 1
  fi
  echo "ok"
  FM+=("-fmodule-file=fish.$m=$OUT/pcm/fish.$m.pcm")
done

echo "==> 2/4 生成对象代码"
FM=()
for m in "${MODULES[@]}"; do
  clang++ -std=c++20 "${FM[@]}" -c "$OUT/pcm/fish.$m.pcm" -o "$OUT/obj/fish.$m.o"
  FM+=("-fmodule-file=fish.$m=$OUT/pcm/fish.$m.pcm")
done

echo "==> 3/4 编译主程序与测试"
clang++ -std=c++20 "${INC[@]}" "${FM[@]}" -c src/main.cpp -o "$OUT/obj/main.o"

TESTS=(test_all test_e2e test_protocol test_lifecycle test_memory)
COMPILED=()
for t in "${TESTS[@]}"; do
  [ -f "tests/$t.cpp" ] || continue
  printf "    %-16s" "$t"
  if clang++ -std=c++20 "${INC[@]}" "${FM[@]}" -c "tests/$t.cpp" -o "$OUT/obj/$t.o" \
      > "$OUT/obj/$t.err" 2>&1; then
    echo "ok"
    COMPILED+=("$t")
  else
    echo "FAILED"
    head -8 "$OUT/obj/$t.err" | sed 's/^/       /'
    exit 1
  fi
done

echo "==> 4/4 链接"
LIBS=(-L"$PREFIX/lib" -lcurl -lsqlite3 -lpthread -Wl,-rpath,"$PREFIX/lib")
clang++ "$OUT/obj/main.o" "$OUT"/obj/fish.*.o -o "$OUT/super-feiyu" "${LIBS[@]}"
for t in "${COMPILED[@]}"; do
  clang++ "$OUT/obj/$t.o" "$OUT"/obj/fish.*.o -o "$OUT/$t" "${LIBS[@]}"
done

echo
echo "构建完成:"
for b in super-feiyu "${COMPILED[@]}"; do
  [ -x "$OUT/$b" ] && echo "  $OUT/$b  ($(du -h "$OUT/$b" | cut -f1))"
done
