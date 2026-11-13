#!/usr/bin/env bash
# 统计各测试二进制的实际 PASS 数。
#
# test_e2e 需要一个 mock API 在 18080 上；没有它就只跑到启动检查，
# 数出 2 项而不是 6 项——曾经因此报出 254 而不是 258。
# 这里自动起 mock（若 tests/mock_server.py 存在且端口空闲），跑完再收掉。
cd ~/super-feiyu || exit 1
export LD_LIBRARY_PATH="$HOME/local/lib"

MOCK_PID=""
if [ -f tests/mock_server.py ] && \
   ! curl -s -m 1 -o /dev/null http://127.0.0.1:18080/ 2>/dev/null; then
  python3 tests/mock_server.py 18080 > /tmp/count_mock.log 2>&1 &
  MOCK_PID=$!
  for _ in $(seq 1 20); do
    curl -s -m 1 -o /dev/null http://127.0.0.1:18080/ 2>/dev/null && break
    sleep 0.3
  done
fi
cleanup() {
  [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
  rm -rf /tmp/fish_e2e
}
trap cleanup EXIT

total=0
for t in test_all test_protocol test_lifecycle test_memory test_e2e; do
  if [ -x "build/$t" ]; then
    n=$(./build/$t 2>/dev/null | grep -c '\[PASS\]')
    printf '%-16s %s\n' "$t" "$n"
    total=$((total + n))
  else
    printf '%-16s %s\n' "$t" "未构建"
  fi
done
echo "----------------------"
echo "合计 $total"
