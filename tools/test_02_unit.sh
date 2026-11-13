#!/usr/bin/env bash
# 全量测试 2/9：所有测试套件，逐项统计
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

echo "════════ 2.1 逐个跑测试套件"
TOTAL_PASS=0
TOTAL_FAIL=0
FAILED_SUITES=()

# 只跑不需要外部服务的套件；e2e 要 mock API，单独在 test_02b_e2e.sh 里跑。
# 原列表含 test_policy / test_transport，两个文件早已删除，只剩「未构建」噪音。
for t in test_all test_protocol test_lifecycle test_memory; do
  if [ ! -x "build/$t" ]; then
    echo "  $t: 未构建"
    FAILED_SUITES+=("$t(未构建)")
    continue
  fi
  ./build/$t > "/tmp/t2_$t.log" 2>&1
  rc=$?
  p=$(grep -c '^\[PASS\]' "/tmp/t2_$t.log" || true)
  f=$(grep -c '^\[FAIL\]' "/tmp/t2_$t.log" || true)
  TOTAL_PASS=$((TOTAL_PASS + p))
  TOTAL_FAIL=$((TOTAL_FAIL + f))
  if [ "$rc" -ne 0 ] || [ "$f" -gt 0 ]; then
    FAILED_SUITES+=("$t")
    printf "  ✗ %-16s PASS=%-4s FAIL=%-3s rc=%s\n" "$t" "$p" "$f" "$rc"
    grep '^\[FAIL\]' "/tmp/t2_$t.log" | head -5
  else
    printf "  ✓ %-16s PASS=%-4s\n" "$t" "$p"
  fi
done

echo
echo "════════ 2.2 汇总"
echo "  断言通过: $TOTAL_PASS"
echo "  断言失败: $TOTAL_FAIL"
echo "  失败套件: ${FAILED_SUITES[*]:-无}"

echo
echo "════════ 2.3 每个套件的断言主题"
for t in test_all test_protocol test_lifecycle test_memory; do
  [ -f "/tmp/t2_$t.log" ] || continue
  echo "  --- $t"
  grep -E '^=== ' "/tmp/t2_$t.log" | sed 's/^/      /' | head -20
done

echo
echo "════════ 2.4 检查测试是否产生副作用"
echo "  data/memory/notes 文件数: $(find data/memory/notes -type f 2>/dev/null | wc -l)"
echo "  data/persona 文件数: $(find data/persona -type f 2>/dev/null | wc -l)"
echo "  临时测试目录残留:"
ls -d /tmp/fish_*_test 2>/dev/null | head -5 || echo "    (无)"
