#!/usr/bin/env bash
set -u
echo "=== 服务: $(systemctl --user is-active super-feiyu)"
echo "=== 监听:"
ss -tlnp 2>/dev/null | grep 8765 || echo "  (8765 未监听)"
echo "=== api 日志（近 5 分钟）:"
journalctl --user -u super-feiyu --since '5 minutes ago' --no-pager 2>/dev/null \
  | grep 'api' | tail -5
echo "=== 本机 curl:"
TOKEN=$(cat "$HOME/super-feiyu/.api_token")
curl -s -m 5 -o /dev/null -w "  127.0.0.1:8765/healthz -> %{http_code}\n" http://127.0.0.1:8765/healthz
curl -s -m 5 -o /dev/null -w "  100.64.0.5:8765/healthz -> %{http_code}\n" http://100.64.0.5:8765/healthz
