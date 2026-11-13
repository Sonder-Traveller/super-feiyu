#!/usr/bin/env bash
# 全量验证：构建 + 各层测试 + 端到端 + 协议 + 真实免费 API。
# 用法: ./verify.sh [--no-network]
set -uo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SRC_DIR"

PREFIX="${PREFIX:-$HOME/local}"
export LD_LIBRARY_PATH="$PREFIX/lib"
NETWORK=1
[[ "${1:-}" == "--no-network" ]] && NETWORK=0

PASS=0
FAIL=0
declare -a FAILED

check() {
  local name="$1" rc="$2"
  if [ "$rc" -eq 0 ]; then
    printf '  \033[32m✓\033[0m %s\n' "$name"
    PASS=$((PASS+1))
  else
    printf '  \033[31m✗\033[0m %s\n' "$name"
    FAIL=$((FAIL+1))
    FAILED+=("$name")
  fi
}

echo "════════ 1. 构建 ════════"
./build.sh 2>&1 | tail -6
check "构建" $?

echo
echo "════════ 2. 单元测试 ════════"
./build/test_all > /tmp/v_t1.log 2>&1
check "单元测试 ($(grep -c '^\[PASS\]' /tmp/v_t1.log) 项)" $?
grep '^\[FAIL\]' /tmp/v_t1.log || true

echo
echo "════════ 3. 协议测试 ════════"
./build/test_protocol > /tmp/v_t2.log 2>&1
check "协议测试 ($(grep -c '^\[PASS\]' /tmp/v_t2.log) 项)" $?
grep '^\[FAIL\]' /tmp/v_t2.log || true

echo
echo "════════ 3b. 生命周期测试 ════════"
./build/test_lifecycle > /tmp/v_t2b.log 2>&1
check "生命周期测试 ($(grep -c '^\[PASS\]' /tmp/v_t2b.log) 项)" $?
grep '^\[FAIL\]' /tmp/v_t2b.log || true

echo
echo "════════ 4. 桩模式自检 ════════"
./build/super-feiyu --self-test > /tmp/v_t3.log 2>&1
check "被动策略与流水线" $?

echo
echo "════════ 5. 端到端记忆闭环（mock API）════════"
python3 tests/mock_server.py 18080 > /tmp/v_mock.log 2>&1 &
MOCK=$!
sleep 2

cat > /tmp/v_e2e.toml <<'EOF'
[capability.chat]
provider = "openai"
base_url = "http://127.0.0.1:18080"
api_key = "test-key"
model = "deepseek-flash"

[capability.embedding]
provider = "siliconflow"
base_url = "http://127.0.0.1:18080/v1"
api_key = "test-key"
model = "BAAI/bge-m3"

[memory]
notes_dir = "/tmp/v_run/notes"
db_path = "/tmp/v_run/i.db"
EOF

rm -rf /tmp/v_run
./build/test_e2e /tmp/v_e2e.toml > /tmp/v_t4.log 2>&1
check "巩固 → 落盘 → 召回 → 索引重建" $?

echo
echo "════════ 6. 通道收消息（mock OneBot WS）════════"
python3 - <<'PY' > /tmp/v_ob.log 2>&1 &
import sys, threading, socket
sys.path.insert(0, "tests")
import mock_onebot_ws as m
sent = []
threading.Thread(target=m.http_server, args=(18086, sent), daemon=True).start()
events = [
  {"post_type":"message","message_type":"group","group_id":1,"user_id":1,
   "message_id":1,"time":0,"sender":{"nickname":"A"},
   "message":[{"type":"text","data":{"text":"未艾特"}}]},
  {"post_type":"message","message_type":"group","group_id":1,"user_id":2,
   "message_id":2,"time":0,"sender":{"nickname":"B"},
   "message":[{"type":"at","data":{"qq":"10086"}},{"type":"text","data":{"text":" 艾特了"}}]},
]
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 18085)); srv.listen(4)
while True:
    c, a = srv.accept()
    threading.Thread(target=m.handle_client, args=(c, a, events), daemon=True).start()
PY
OB=$!
sleep 2

cat > /tmp/v_ob.toml <<'EOF'
[channel.qq_onebot]
enabled = true
ws_url = "ws://127.0.0.1:18085"
http_url = "http://127.0.0.1:18086"
self_id = "10086"

[capability.chat]
provider = "openai"
base_url = "http://127.0.0.1:18080"
api_key = "test-key"
model = "deepseek-flash"

[capability.embedding]
provider = "siliconflow"
base_url = "http://127.0.0.1:18080/v1"
api_key = "test-key"
model = "BAAI/bge-m3"

[memory]
notes_dir = "/tmp/v_ob_run/notes"
db_path = "/tmp/v_ob_run/i.db"

[persona]
persona_file = "persona/persona.md"
rules_file = "persona/rules.md"
EOF

rm -rf /tmp/v_ob_run
timeout 8 ./build/super-feiyu --config /tmp/v_ob.toml > /tmp/v_t5.log 2>&1
REPLIED=$(grep -c '"event":"replied"' /tmp/v_t5.log)
SENDGRP=$(grep -c "send_group_msg" /tmp/v_ob.log)
[ "$REPLIED" -ge 1 ] && [ "$SENDGRP" -ge 1 ]
check "WS 收消息 → @ 判定 → HTTP 发送（回 $REPLIED 条）" $?

kill $MOCK $OB 2>/dev/null

echo
echo "════════ 7. 配置校验 ════════"
cat > /tmp/v_bad.toml <<'EOF'
[channel.qq_onebot]
enabled = true
ws_url = "ws://127.0.0.1:3001"
[capability.chat]
provider = "deepseek"
api_key = ""
[persona]
persona_file = "persona/persona.md"
rules_file = "persona/rules.md"
EOF
OUT=$(./build/super-feiyu --config /tmp/v_bad.toml 2>&1)
echo "$OUT" | grep -q "config_invalid"
check "缺配置拒绝启动" $?

echo
echo "════════ 8. 真实免费 API ════════"
if [ "$NETWORK" -eq 1 ] && [ -f .env.local ]; then
  SK=$(grep '^SILICONFLOW_API_KEY=' .env.local | cut -d= -f2- | tr -d "[:space:]")
  if [ -n "$SK" ]; then
    HTTP=$(curl -s -o /tmp/v_emb.json -w "%{http_code}" --max-time 40 \
      -X POST "https://api.siliconflow.cn/v1/embeddings" \
      -H "Authorization: Bearer $SK" -H "Content-Type: application/json" \
      -d '{"model":"BAAI/bge-m3","input":"验证"}')
    [ "$HTTP" = "200" ] && grep -q '"embedding"' /tmp/v_emb.json
    check "真实 embedding（bge-m3，免费模型）" $?
  else
    echo "  - 跳过（.env.local 无 key）"
  fi
else
  echo "  - 跳过（--no-network 或无 .env.local）"
fi

echo
echo "════════ 结果 ════════"
printf '通过 %s 项，失败 %s 项\n' "$PASS" "$FAIL"
if [ "$FAIL" -gt 0 ]; then
  printf '失败项：\n'
  for f in "${FAILED[@]}"; do printf '  - %s\n' "$f"; done
  exit 1
fi
printf '\033[32m全部通过\033[0m\n'
