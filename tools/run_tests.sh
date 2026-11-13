#!/usr/bin/env bash
# 跑全部测试二进制
set -u
cd "$HOME/super-feiyu" || exit 1

fail=0
for t in test_all test_protocol test_lifecycle test_memory test_policy test_transport; do
  if [ ! -x "build/$t" ]; then
    echo "=== $t  (未构建，跳过)"
    continue
  fi
  echo "=== $t"
  if ./build/$t 2>&1 | tail -4; then
    :
  else
    fail=1
    echo "  ^^^ 失败"
  fi
  echo
done

echo "失败标记: $fail"
