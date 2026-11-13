#!/usr/bin/env bash
# 全量测试 1/9：构建 + 静态检查
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

echo "════════ 1.1 全量构建"
rm -rf /tmp/full_build
OUT=/tmp/full_build ./build.sh > /tmp/build.log 2>&1
BC=$?
echo "退出码: $BC"
echo "模块数: $(grep -c 'ok$' /tmp/build.log)"
echo "产物:"
ls -la /tmp/full_build/super-feiyu 2>/dev/null | awk '{printf "  %s  %s\n", $5, $9}'

echo
echo "════════ 1.2 编译警告/错误统计"
grep -ciE 'warning:' /tmp/build.log || echo 0
echo "警告明细（前 10 条）:"
grep -iE 'warning:' /tmp/build.log | head -10
echo "错误明细（前 5 条）:"
grep -iE 'error:' /tmp/build.log | head -5

echo
echo "════════ 1.3 二进制依赖"
ldd /tmp/full_build/super-feiyu 2>/dev/null | head -20
echo "--- 是否静态链 curl/sqlite:"
ldd /tmp/full_build/super-feiyu 2>/dev/null | grep -cE 'libcurl|libsqlite3' || true

echo
echo "════════ 1.4 源码规模"
find src -name '*.cppm' | wc -l | xargs echo "  模块文件:"
wc -l src/*.cppm src/main.cpp 2>/dev/null | tail -1
