#!/usr/bin/env bash
# 排查 API 未监听
set -u
echo "=== 服务状态"
systemctl --user is-active super-feiyu

echo "=== [api] 配置"
grep -A4 '^\[api\]' "$HOME/super-feiyu/config.toml"

echo "=== api 相关日志（近 15 分钟）"
journalctl --user -u super-feiyu --since '15 minutes ago' --no-pager 2>/dev/null \
  | grep -E 'module":"api' | tail -8

echo "=== 错误日志"
journalctl --user -u super-feiyu --since '15 minutes ago' --no-pager 2>/dev/null \
  | grep -E 'level":"(error|warn)' | tail -6

echo "=== 监听端口"
ss -tlnp 2>/dev/null | grep -E '8765' || echo "(8765 未监听)"

echo "=== 进程"
pgrep -af 'build/super-feiyu' | head -2
