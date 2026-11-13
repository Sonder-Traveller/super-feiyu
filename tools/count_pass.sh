#!/usr/bin/env bash
# 统计各测试二进制的实际 PASS 数。
cd ~/super-feiyu || exit 1
export LD_LIBRARY_PATH="$HOME/local/lib"
total=0
for t in test_all test_protocol test_lifecycle test_memory test_e2e; do
  if [ -x "build/$t" ]; then
    n=$(./build/$t 2>/dev/null | grep -c '\[PASS\]')
    printf '%-16s %s\n' "$t" "$n"
    total=$((total + n))
  fi
done
echo "----------------------"
echo "合计 $total"
