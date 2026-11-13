#!/usr/bin/env bash
# 修复后全量回归：构建 + 所有测试（含 e2e 需要 mock）
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

echo "════════ 构建"
./build.sh > /tmp/fb.log 2>&1
echo "  退出码: $?  模块: $(grep -c 'ok$' /tmp/fb.log)  测试: $(grep -c 'ok$' /tmp/fb.log)"

echo
echo "════════ 单元测试（不需要外部服务）"
TP=0; TF=0
for t in test_all test_protocol test_lifecycle test_memory; do
  ./build/$t > "/tmp/f_$t.log" 2>&1
  rc=$?
  p=$(grep -c '^\[PASS\]' "/tmp/f_$t.log" || true)
  f=$(grep -c '^\[FAIL\]' "/tmp/f_$t.log" || true)
  TP=$((TP+p)); TF=$((TF+f))
  [ $rc -eq 0 ] && [ "$f" -eq 0 ] && \
    printf "  ✓ %-18s PASS=%s\n" "$t" "$p" || \
    printf "  ✗ %-18s PASS=%s FAIL=%s rc=%s\n" "$t" "$p" "$f" "$rc"
done

echo
echo "════════ e2e（需要 mock）"
pkill -f 'tests/mock_server.py' 2>/dev/null; sleep 1
python3 tests/mock_server.py 18080 > /tmp/mock2.log 2>&1 &
sleep 2
rm -rf /tmp/fish_e2e
./build/test_e2e > /tmp/f_e2e.log 2>&1
rc=$?
p=$(grep -c '^\[PASS\]' /tmp/f_e2e.log || true)
f=$(grep -c '^\[FAIL\]' /tmp/f_e2e.log || true)
TP=$((TP+p)); TF=$((TF+f))
[ $rc -eq 0 ] && [ "$f" -eq 0 ] && \
  printf "  ✓ %-18s PASS=%s\n" "test_e2e" "$p" || \
  printf "  ✗ %-18s PASS=%s FAIL=%s rc=%s\n" "test_e2e" "$p" "$f" "$rc"
grep '^\[FAIL\]' /tmp/f_e2e.log | sed 's/^/      /' || true
pkill -f 'tests/mock_server.py' 2>/dev/null
rm -rf /tmp/fish_e2e

echo
echo "════════ 汇总"
echo "  通过: $TP"
echo "  失败: $TF"
echo "  二进制校验（都在构建列表里吗）:"
for b in build/test_*; do
  n=$(basename "$b")
  grep -q "$n" build.sh && echo "    ✓ $n 在构建列表" || echo "    ⚠ $n 是残留"
done

echo
echo "════════ 修复项验证"
echo "  TOML 语法错误是否被捕获:"
printf 'this is not toml\n' > /tmp/badcfg.toml
./build/super-feiyu --config /tmp/badcfg.toml 2>&1 | grep -oE 'config_parse_error[^"]*' | head -2 | sed 's/^/    /'
printf '[broken\n' > /tmp/badcfg2.toml
./build/super-feiyu --config /tmp/badcfg2.toml 2>&1 | grep -oE 'unclosed section header[^"]*' | head -1 | sed 's/^/    /'
rm -f /tmp/badcfg*.toml
