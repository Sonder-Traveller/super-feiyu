#!/usr/bin/env bash
# 验证控制面 API。
#
# 注意：token 必须写进 systemd unit（Environment=）或 EnvironmentFile，
# 直接 export 到 shell 里 systemctl restart 是拿不到的——
# 这是第一次测试卡住的原因。
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

UNIT="$HOME/.config/systemd/user/super-feiyu.service"
CFG=config.toml
TOKEN="live-test-token-$$"

cp "$CFG" /tmp/cfg.bak
cp "$UNIT" /tmp/unit.bak

cleanup() {
  cp /tmp/cfg.bak "$CFG"
  cp /tmp/unit.bak "$UNIT"
  systemctl --user daemon-reload >/dev/null 2>&1
  systemctl --user restart super-feiyu >/dev/null 2>&1
  echo
  echo "[已恢复配置与 unit]"
}
trap cleanup EXIT

# 临时开 API
if grep -q '^\[api\]' "$CFG"; then
  sed -i 's/^port = .*/port = 18766/' "$CFG"
else
  printf '\n[api]\nport = 18766\nbind = "127.0.0.1"\nauth = true\n' >> "$CFG"
fi

# token 写进 unit（这是关键：systemd 不继承 shell 环境）
if grep -q '^Environment=FEIYU_API_TOKEN=' "$UNIT"; then
  sed -i "s|^Environment=FEIYU_API_TOKEN=.*|Environment=FEIYU_API_TOKEN=$TOKEN|" "$UNIT"
else
  sed -i "/^\[Service\]/a Environment=FEIYU_API_TOKEN=$TOKEN" "$UNIT"
fi

systemctl --user daemon-reload
systemctl --user restart super-feiyu
sleep 6

echo "=== 服务: $(systemctl --user is-active super-feiyu)"
journalctl --user -u super-feiyu --since '20 seconds ago' --no-pager 2>/dev/null \
  | grep -iE '"module":"api"' | tail -3

B="http://127.0.0.1:18766"
c() { curl -s -m 5 -w ' [%{http_code}]\n' "$@"; }

echo
echo "1) healthz 免鉴权:";        c "$B/healthz"
echo "2) 无 token → 401:";        c "$B/v1/persona/default"
echo "3) 错 token → 401:";        c -H "Authorization: Bearer wrong" "$B/v1/persona/default"
echo "4) 正确 token 读:";         c -H "Authorization: Bearer $TOKEN" "$B/v1/persona/default"
echo "5) 写全局人格:";            c -X PUT -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{"persona":"测试全局人格"}' "$B/v1/persona/default"
echo "6) 回读:";                  c -H "Authorization: Bearer $TOKEN" "$B/v1/persona/default"
echo "7) 设会话人格:";            c -X PUT -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{"persona":"会话专属"}' "$B/v1/persona/session?conv_id=live-test"
echo "8) 读会话人格:";            c -H "Authorization: Bearer $TOKEN" "$B/v1/persona/session?conv_id=live-test"
echo "9) 缺 conv_id → 400:";      c -H "Authorization: Bearer $TOKEN" "$B/v1/persona/session"
echo "10) 清覆盖:";               c -X DELETE -H "Authorization: Bearer $TOKEN" "$B/v1/persona/session?conv_id=live-test"
echo "11) 清后回落全局:";         c -H "Authorization: Bearer $TOKEN" "$B/v1/persona/session?conv_id=live-test"
echo "12) 未知路径 → 404:";       c -H "Authorization: Bearer $TOKEN" "$B/v1/nope"

echo
echo "=== 落盘:"
ls -la data/persona/ data/persona/sessions/ 2>/dev/null
echo "default.md: $(cat data/persona/default.md 2>/dev/null)"
