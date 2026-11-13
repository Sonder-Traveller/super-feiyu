#!/usr/bin/env bash
# 诊断 clang 崩溃：逐个模块单独预编译，每次都是全新进程，避免状态污染
set -u
cd "$HOME/super-feiyu" || exit 1

PREFIX="$HOME/local"
INC=(-Iinclude -I"$PREFIX/include" -I"$PREFIX/include/x86_64-linux-gnu" -Isrc \
     -Ithird_party/thread-pool/pool/include)
MODULES=(core log config memory capability probe media image meme persona api session tool guard ws qq_ws onebot_ws channels dispatcher orchestrator)

PCM=/tmp/pcm_seq
rm -rf "$PCM"; mkdir -p "$PCM"

echo "clang 版本: $(clang++ --version | head -1)"
echo

FM=()
for m in "${MODULES[@]}"; do
  printf "%-14s " "$m"
  # 只跑一次，捕获所有输出（stdout+stderr）
  if clang++ -std=c++20 "${INC[@]}" "${FM[@]}" \
      --precompile "src/fish.$m.cppm" -o "$PCM/fish.$m.pcm" > "$PCM/$m.log" 2>&1; then
    echo "ok"
    FM+=("-fmodule-file=fish.$m=$PCM/fish.$m.pcm")
  else
    rc=$?
    echo "FAILED (rc=$rc, $(stat -c%s "$PCM/$m.log" 2>/dev/null) 字节日志)"
    echo "--- 最后 8 行:"
    tail -8 "$PCM/$m.log" | sed 's/^/    /'
    echo "--- 累计模块参数数: ${#FM[@]}"
    exit 1
  fi
done

echo
echo "全部 ${#MODULES[@]} 个模块预编译成功"
