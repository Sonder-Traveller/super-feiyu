#!/usr/bin/env bash
# 统计各测试套件的断言数
#
# 曾经的坑：这个脚本列了 test_policy / test_transport，那两个文件已删，
# 于是每次只打印「未构建」并跳过——看起来"跑过了"，实际什么都没跑。
# 现在列表与 build.sh 的 TESTS 数组保持一致，多一个少一个都会显出来。
set -u
cd "$HOME/super-feiyu" || exit 1

total=0
missing=0
for t in test_all test_protocol test_lifecycle test_memory test_e2e; do
  if [ ! -x "build/$t" ]; then
    echo "$t: 未构建"
    missing=$((missing + 1))
    continue
  fi
  n=$(./build/$t 2>&1 | grep -c '\[PASS\]')
  echo "$t: $n"
  total=$((total + n))
done
echo "合计: $total"
[ "$missing" -gt 0 ] && echo "（有 $missing 个套件未构建，先跑 ./build.sh）"
exit 0
