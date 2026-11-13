#!/usr/bin/env bash
# 逐模块复现预编译，找出 probe 失败的真因
set -u
cd "$HOME/super-feiyu" || exit 1

INC=(-Iinclude -I"$HOME/local/include" -I"$HOME/local/include/x86_64-linux-gnu" -Isrc \
     -Ithird_party/thread-pool/pool/include)
MODULES=(core log config memory capability probe)
PCM=/tmp/pcmtest
rm -rf "$PCM"; mkdir -p "$PCM"

FM=()
for m in "${MODULES[@]}"; do
  printf "%-12s " "$m"
  if clang++ -std=c++20 "${INC[@]}" "${FM[@]}" \
      --precompile "src/fish.$m.cppm" -o "$PCM/fish.$m.pcm" 2>"$PCM/$m.err"; then
    echo "ok"
  else
    echo "FAILED"
    echo "--- 错误:"
    head -6 "$PCM/$m.err"
    echo "--- 当时传给编译器的模块参数:"
    printf '  %s\n' "${FM[@]}"
    echo "--- 已生成的 pcm:"
    ls -la "$PCM"/*.pcm 2>/dev/null | awk '{print "  "$5, $9}'
    exit 1
  fi
  FM+=("-fmodule-file=fish.$m=$PCM/fish.$m.pcm")
done
