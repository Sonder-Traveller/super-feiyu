#!/usr/bin/env bash
# 跑全部测试二进制
#
# 列表与 build.sh 的 TESTS 数组一致。曾经列了已删除的 test_policy /
# test_transport，每次都打印「未构建，跳过」假装跑过了。
set -u
cd "$HOME/super-feiyu" || exit 1
export LD_LIBRARY_PATH="$HOME/local/lib"

fail=0
for t in test_all test_protocol test_lifecycle test_memory test_e2e; do
  if [ ! -x "build/$t" ]; then
    echo "=== $t  (未构建，跳过)"
    fail=1
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
exit "$fail"
