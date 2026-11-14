#!/usr/bin/env bash
# 恢复 0.0.0.0 绑定，检查 WSLg 可用性
set -u
cd "$HOME/super-feiyu" || exit 1

sed -i 's/^bind = .*/bind = "0.0.0.0"/' config.toml
systemctl --user restart super-feiyu >/dev/null 2>&1
sleep 5

echo "=== API"
curl -s -m 3 -o /dev/null -w "  127.0.0.1:8765 -> %{http_code}\n" http://127.0.0.1:8765/healthz
ss -tln 2>/dev/null | grep 8765 | sed 's/^/  /'

echo
echo "=== WSLg"
echo "  DISPLAY=${DISPLAY:-未设置}"
echo "  WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-未设置}"
echo "  /mnt/wslg 内容:"
ls /mnt/wslg 2>/dev/null | head -5 | sed 's/^/    /' || echo "    (不存在)"

echo
echo "=== X11 socket"
ls /tmp/.X11-unix 2>/dev/null | sed 's/^/    /' || echo "    (无)"
