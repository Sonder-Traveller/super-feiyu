#!/usr/bin/env bash
# 检查 API 在各地址上的可达性，并输出手机端配置信息
set -u
cd "$HOME/super-feiyu" || exit 1

TOKEN=$(cat .api_token)

echo "=== API 可达性"
for ip in 127.0.0.1 100.64.0.5 192.168.1.134; do
  code=$(curl -s -m 3 -o /dev/null -w '%{http_code}' "http://$ip:8765/healthz" 2>/dev/null)
  printf "  %-16s -> %s\n" "$ip" "${code:-失败}"
done

echo
echo "=== 手机端需要填的信息"
echo "  主机: 100.64.0.5   （Tailscale 地址，手机装了 Tailscale 就能用）"
echo "  备选: 192.168.1.134（同一 WiFi 时用这个）"
echo "  端口: 8765"
echo "  Token: $TOKEN"
echo
echo "  Token 已保存到: ~/super-feiyu/.api_token"

echo
echo "=== 验证带 token 的请求"
code=$(curl -s -m 3 -o /dev/null -w '%{http_code}' \
  -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8765/v1/persona/default)
echo "  GET /v1/persona/default -> $code"
curl -s -m 3 -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8765/v1/persona/default
echo
