#!/usr/bin/env bash
# 让控制面 API 可从局域网访问：绑 0.0.0.0 + 放行防火墙
set -euo pipefail
cd "$HOME/super-feiyu" || exit 1

CFG=config.toml
PORT=8765

echo "════════ 1. 改绑定地址"
sed -i "s/^bind = .*/bind = \"0.0.0.0\"/" "$CFG"
sed -n '/^\[api\]/,/^$/p' "$CFG"

echo
echo "════════ 2. 放行端口（若启用了 ufw）"
if command -v ufw >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
  sudo ufw allow "$PORT"/tcp 2>/dev/null && echo "  ufw 已放行 $PORT" || echo "  ufw 放行失败"
else
  echo "  跳过（无 ufw 或无免密 sudo）"
fi

echo
echo "════════ 3. 重启"
systemctl --user restart super-feiyu
sleep 6
echo "  状态: $(systemctl --user is-active super-feiyu)"

echo
echo "════════ 4. 监听检查"
ss -tlnp 2>/dev/null | grep ":$PORT" | sed 's/^/  /'

echo
echo "════════ 5. 从 WSL 内用外部 IP 访问"
TOKEN=$(cat .api_token)
for ip in 127.0.0.1 100.64.0.5 192.168.1.134; do
  code=$(curl -s -m 5 -o /dev/null -w '%{http_code}' "http://$ip:$PORT/healthz" 2>/dev/null || echo "连接失败")
  printf "  %-16s -> %s\n" "$ip" "$code"
done

echo
echo "════════ 6. 从 Windows 侧访问（模拟手机同网段）"
