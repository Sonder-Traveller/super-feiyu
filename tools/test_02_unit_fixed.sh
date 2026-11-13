#!/usr/bin/env bash
# 全量测试 2/9（修正版）：只跑真正构建出来的测试
#
# 上一版失误：test_memory / test_policy / test_transport 三个二进制
# 不在 build.sh 的构建列表里，是 00:18 的历史残留。
# 跑它们得到的是过期结果，必须排除。
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

echo "════════ 0. 先确认哪些测试真的会被构建"
BUILT=$(grep -oE 'test_[a-z]+' build.sh | sort -u | tr '\n' ' ')
echo "  build.sh 构建的目标: $BUILT"
echo "  build/ 目录里的二进制:"
ls -la build/test_* 2>/dev/null | awk '{printf "    %s  %s\n", $6" "$7" "$8, $9}'

echo
echo "════════ 0b. 检查哪些是残留（源文件比二进制新，或不在构建列表）"
for bin in build/test_*; do
  [ -x "$bin" ] || continue
  name=$(basename "$bin")
  short=${name#test_}
  if ! grep -q "test_$short" build.sh; then
    echo "  ⚠ $name: 不在 build.sh 构建列表 → 残留"
  elif [ tests/test_$short.cpp -nt "$bin" ]; then
    echo "  ⚠ $name: 源文件比二进制新 → 过期"
  else
    echo "  ✓ $name: 有效"
  fi
done

echo
echo "════════ 1. 清理并按 build.sh 重新构建"
rm -f build/test_all build/test_e2e build/test_protocol build/test_lifecycle
rm -f build/test_memory
rm -f build/test_policy build/test_transport  # 源文件已删，清掉历史残留
./build.sh > /tmp/build2.log 2>&1
echo "  构建退出码: $?"
ls build/test_* 2>/dev/null | sed 's/^/    /'

echo
echo "════════ 2. 只跑实际构建出来的测试"
TOTAL=0; FAILS=0
for bin in build/test_all build/test_e2e build/test_protocol build/test_lifecycle build/test_memory; do
  [ -x "$bin" ] || { echo "  $bin 不存在"; continue; }
  name=$(basename "$bin")
  ./"$bin" > "/tmp/r_$name.log" 2>&1
  rc=$?
  p=$(grep -c '^\[PASS\]' "/tmp/r_$name.log" || true)
  f=$(grep -c '^\[FAIL\]' "/tmp/r_$name.log" || true)
  TOTAL=$((TOTAL+p)); FAILS=$((FAILS+f))
  if [ $rc -eq 0 ] && [ "$f" -eq 0 ]; then
    printf "  ✓ %-18s PASS=%-4s rc=%s\n" "$name" "$p" "$rc"
  else
    printf "  ✗ %-18s PASS=%-4s FAIL=%-3s rc=%s\n" "$name" "$p" "$f" "$rc"
  fi
done

echo
echo "  真实断言总数: $TOTAL"
echo "  失败: $FAILS"

echo
echo "════════ 3. 未被构建的测试文件（技术债）"
for f in tests/test_*.cpp; do
  short=$(basename "$f" .cpp)
  if ! grep -q "$short" build.sh; then
    echo "  ⚠ $f 未纳入构建，内容可能已失效"
    # 看它引用的符号是否还存在
    grep -oE 'fish::[a-z_]+::[A-Za-z_]+' "$f" 2>/dev/null | sort -u | head -5 | sed 's/^/      引用: /'
  fi
done
