#!/usr/bin/env bash
# 统计各测试套件的断言数
set -u
cd "$HOME/super-feiyu" || exit 1

total=0
for t in test_all test_protocol test_lifecycle test_memory test_policy test_transport; do
  if [ ! -x "build/$t" ]; then
    echo "$t: 未构建"
    continue
  fi
  n=$(./build/$t 2>&1 | grep -c '\[PASS\]')
  echo "$t: $n"
  total=$((total + n))
done
echo "合计: $total"
