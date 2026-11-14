#!/usr/bin/env bash
# 查服务为什么反复重启
set -u
echo "=== 最近 40 条完整日志（含 systemd）"
journalctl --user -u super-feiyu --since '20 minutes ago' --no-pager 2>/dev/null | tail -40

echo
echo "=== systemd 重启统计"
systemctl --user show super-feiyu -p NRestarts -p Restart -p RestartSec -p ExecMainStartTimestamp --value 2>/dev/null | sed 's/^/  /'

echo
echo "=== 退出码历史"
journalctl --user -u super-feiyu --since '20 minutes ago' --no-pager 2>/dev/null \
  | grep -E 'Main process exited|Failed with result|Scheduled restart|start request repeated' | tail -10
