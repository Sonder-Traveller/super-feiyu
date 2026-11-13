#!/usr/bin/env bash
# 服务健康检查
set -u
echo "=== 状态"
systemctl --user is-active super-feiyu

echo "=== 启动时间"
systemctl --user show super-feiyu -p ActiveEnterTimestamp --value

echo "=== 内存"
pid=$(pgrep -f 'super-feiyu --config' | head -1)
if [ -n "$pid" ]; then
  rss=$(ps -o rss= -p "$pid" | tr -d ' ')
  awk -v r="$rss" 'BEGIN{printf "  %.1f MB\n", r/1024}'
  echo "  pid=$pid"
else
  echo "  未找到进程"
fi

echo "=== 线程数"
[ -n "$pid" ] && ls /proc/"$pid"/task 2>/dev/null | wc -l

echo "=== 最近日志"
journalctl --user -u super-feiyu -n 15 --no-pager | tail -10

echo "=== 错误统计（本次启动）"
journalctl --user -u super-feiyu --since "$(systemctl --user show super-feiyu -p ActiveEnterTimestamp --value)" --no-pager 2>/dev/null | grep -c '"level":"error"' || echo 0
