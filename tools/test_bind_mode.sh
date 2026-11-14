#!/usr/bin/env bash
# 在 mirrored 网络模式下找到 Windows 能访问的绑定方式
set -u
cd "$HOME/super-feiyu" || exit 1

TOKEN=$(cat .api_token)

test_bind() {
  local bind="$1"
  echo "=== 测试 bind=$bind"
  sed -i "s/^bind = .*/bind = \"$bind\"/" config.toml
  systemctl --user restart super-feiyu >/dev/null 2>&1
  sleep 5

  echo "  ss 监听:"
  ss -tln 2>/dev/null | grep 8765 | sed 's/^/    /' || echo "    (无)"

  for ip in 127.0.0.1 100.64.0.5 192.168.1.134; do
    code=$(curl -s -m 3 -o /dev/null -w '%{http_code}' "http://$ip:8765/healthz" 2>/dev/null)
    printf "    WSL->%-16s %s\n" "$ip" "${code:-连接失败}"
  done
  echo
}

test_bind "0.0.0.0"
test_bind "127.0.0.1"
