#!/usr/bin/env bash
# 全量验证：构建 + 单元测试 + 端到端（含 mock 服务器）。
# 用法: ./verify.sh
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SRC_DIR"

export LD_LIBRARY_PATH="${PREFIX:-$HOME/local}/lib"

echo "════════ 1. 构建 ════════"
./build.sh 2>&1 | tail -6

echo
echo "════════ 2. 单元测试 ════════"
./build/test_all 2>&1 | tail -6

echo
echo "════════ 3. 桩模式自检 ════════"
./build/super-feiyu --self-test 2>&1 | tail -8

echo
echo "════════ 4. 端到端（真实 HTTP + 记忆闭环）════════"
python3 tests/mock_server.py 18080 > /tmp/fish_mock.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 2

cat > /tmp/fish_e2e.toml <<'EOF'
[capability.chat]
provider = "deepseek"
base_url = "http://127.0.0.1:18080"
api_key = "test-key-123456"
model = "deepseek-flash"

[capability.embedding]
provider = "siliconflow"
base_url = "http://127.0.0.1:18080/v1"
api_key = "test-key-123456"
model = "BAAI/bge-m3"
dimension = 1024

[memory]
notes_dir = "/tmp/fish_e2e_run/notes"
db_path = "/tmp/fish_e2e_run/i.db"
EOF

rm -rf /tmp/fish_e2e_run
./build/test_e2e /tmp/fish_e2e.toml 2>&1 | tail -12

echo
echo "════════ 5. 真实 API 链路（走 provider 工厂）════════"
rm -rf /tmp/fish_live_run
sed 's#/tmp/fish_e2e_run#/tmp/fish_live_run#' /tmp/fish_e2e.toml > /tmp/fish_live.toml
./build/super-feiyu --config /tmp/fish_live.toml --live-test 2>&1 | tail -10

echo
echo "════════ 全部验证完成 ════════"
