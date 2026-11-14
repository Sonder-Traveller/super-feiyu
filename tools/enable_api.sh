#!/usr/bin/env bash
# 开启控制面 API：写配置 + 写 token 到 systemd unit + 重启 + 验证
set -euo pipefail
cd "$HOME/super-feiyu" || exit 1

CFG=config.toml
UNIT="$HOME/.config/systemd/user/super-feiyu.service"
TOKEN_FILE="$HOME/super-feiyu/.api_token"
PORT=8765

# ── 1. 生成/复用 token ──
if [ -f "$TOKEN_FILE" ] && [ -s "$TOKEN_FILE" ]; then
  TOKEN=$(cat "$TOKEN_FILE")
  echo "复用已有 token"
else
  TOKEN=$(head -c 32 /dev/urandom | base64 | tr -d '/+=' | head -c 40)
  printf '%s' "$TOKEN" > "$TOKEN_FILE"
  chmod 600 "$TOKEN_FILE"
  echo "生成新 token 并写入 $TOKEN_FILE (600)"
fi

# ── 2. 配置 [api] 段 ──
if grep -q '^\[api\]' "$CFG"; then
  sed -i "s/^port = .*/port = $PORT/" "$CFG"
  sed -i 's/^auth = .*/auth = true/' "$CFG"
  echo "已更新 [api] 段 port=$PORT"
else
  cat >> "$CFG" <<EOF

[api]
port = $PORT
bind = "127.0.0.1"
auth = true
EOF
  echo "已追加 [api] 段 port=$PORT"
fi

# ── 3. token 写进 unit（systemd 不继承 shell 环境）──
if grep -q '^Environment=FEIYU_API_TOKEN=' "$UNIT"; then
  sed -i "s|^Environment=FEIYU_API_TOKEN=.*|Environment=FEIYU_API_TOKEN=$TOKEN|" "$UNIT"
else
  sed -i "/^\[Service\]/a Environment=FEIYU_API_TOKEN=$TOKEN" "$UNIT"
fi
echo "token 已写入 systemd unit"

# ── 4. 重启 ──
systemctl --user daemon-reload
systemctl --user reset-failed super-feiyu 2>/dev/null || true
systemctl --user restart super-feiyu
sleep 6

# ── 5. 验证 ──
echo
echo "=== 服务状态: $(systemctl --user is-active super-feiyu)"
journalctl --user -u super-feiyu --since '30 seconds ago' --no-pager 2>/dev/null \
  | grep -E '"module":"api"' | tail -3 | sed 's/^/    /'

echo
echo "=== 端点自检"
B="http://127.0.0.1:$PORT"
printf "  /healthz          -> %s\n" "$(curl -s -m 5 -o /dev/null -w '%{http_code}' $B/healthz)"
printf "  无 token 读人格   -> %s\n" "$(curl -s -m 5 -o /dev/null -w '%{http_code}' $B/v1/persona/default)"
printf "  有 token 读人格   -> %s\n" "$(curl -s -m 5 -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" $B/v1/persona/default)"
echo "  当前人格: $(curl -s -m 5 -H "Authorization: Bearer $TOKEN" $B/v1/persona/default)"
