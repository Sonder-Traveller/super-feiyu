#!/usr/bin/env bash
# 全量测试 2b：正确启动 mock 后重跑 e2e
#
# 上一次 4 个 FAIL 全是环境问题：e2e 需要 tests/mock_server.py 在 18080。
# 这个脚本验证「环境就绪后 e2e 是否真的过」。
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

echo "════════ 启动 mock 服务"
pkill -f 'tests/mock_server.py' 2>/dev/null
sleep 1

python3 tests/mock_server.py 18080 > /tmp/mock.log 2>&1 &
MOCK_PID=$!
sleep 2

if ! kill -0 "$MOCK_PID" 2>/dev/null; then
  echo "  mock 启动失败:"; cat /tmp/mock.log | head -5; exit 1
fi
echo "  mock 已启动 (pid=$MOCK_PID)"

# 等端口就绪
for i in $(seq 1 20); do
  if curl -s -m 1 -o /dev/null http://127.0.0.1:18080/v1/models 2>/dev/null || \
     curl -s -m 1 -o /dev/null http://127.0.0.1:18080/ 2>/dev/null; then
    echo "  端口 18080 就绪"
    break
  fi
  sleep 0.5
done

echo
echo "════════ 清理旧数据并跑 e2e"
rm -rf /tmp/fish_e2e
./build/test_e2e > /tmp/e2e.log 2>&1
RC=$?

echo "  退出码: $RC"
echo "  PASS: $(grep -c '^\[PASS\]' /tmp/e2e.log || true)"
echo "  FAIL: $(grep -c '^\[FAIL\]' /tmp/e2e.log || true)"
echo
echo "  详细:"
grep -E '^\[PASS\]|^\[FAIL\]|^  ' /tmp/e2e.log | sed 's/^/    /'

echo
echo "════════ 落盘检查（旁路文件权威源）"
echo "  /tmp/fish_e2e 结构:"
find /tmp/fish_e2e -type f 2>/dev/null | sed 's/^/    /' | head -15
echo "  facts.md 内容:"
head -5 /tmp/fish_e2e/notes/*/facts.md 2>/dev/null | sed 's/^/    /'

echo
echo "════════ mock 调用统计"
grep -c 'POST' /tmp/mock.log 2>/dev/null | xargs echo "  请求数:"

pkill -f 'tests/mock_server.py' 2>/dev/null
rm -rf /tmp/fish_e2e
echo "[mock 已停止]"
