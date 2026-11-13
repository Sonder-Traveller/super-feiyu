#!/usr/bin/env bash
# 全量测试 3/9：内存、线程、fd、稳定性
set -uo pipefail
PID=$(pgrep -f 'build/super-feiyu --config' | head -1)
[ -z "$PID" ] && { echo "服务未运行"; exit 1; }

echo "════════ 3.1 当前资源"
echo "  PID: $PID"
rss=$(ps -o rss= -p "$PID" | tr -d ' ')
awk -v r="$rss" 'BEGIN{printf "  RSS: %.1f MB\n", r/1024}'
echo "  线程: $(ls /proc/$PID/task | wc -l)"
echo "  fd:   $(ls /proc/$PID/fd 2>/dev/null | wc -l)"
echo "  VmPeak: $(grep VmPeak /proc/$PID/status | awk '{print $2/1024" MB"}')"
echo "  VmRSS:  $(grep VmRSS /proc/$PID/status | awk '{print $2/1024" MB"}')"

echo
echo "════════ 3.2 fd 明细（按类型）"
ls -l /proc/$PID/fd 2>/dev/null | awk '{print $NF}' | sed 's/[0-9]\+/N/g' | sort | uniq -c | sort -rn | head -10

echo
echo "════════ 3.3 线程名"
for t in /proc/$PID/task/*/comm; do cat "$t" 2>/dev/null; done | sort | uniq -c | sort -rn

echo
echo "════════ 3.4 60 秒内存漂移（每 10 秒采样）"
for i in 1 2 3 4 5 6; do
  r=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ')
  th=$(ls /proc/$PID/task 2>/dev/null | wc -l)
  fd=$(ls /proc/$PID/fd 2>/dev/null | wc -l)
  [ -z "$r" ] && { echo "  进程已退出"; break; }
  awk -v r="$r" -v t="$th" -v f="$fd" -v i="$i" \
    'BEGIN{printf "  [%2ds] RSS=%.1f MB  线程=%d  fd=%d\n", i*10, r/1024, t, f}'
  [ "$i" -lt 6 ] && sleep 10
done

echo
echo "════════ 3.5 错误/警告日志统计"
journalctl --user -u super-feiyu --since '30 minutes ago' --no-pager 2>/dev/null \
  | grep -oE '"level":"[a-z]+"' | sort | uniq -c
echo "  最近警告:"
journalctl --user -u super-feiyu --since '30 minutes ago' --no-pager 2>/dev/null \
  | grep '"level":"warn"' | tail -3 | sed 's/^/    /'
