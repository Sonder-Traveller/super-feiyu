#!/usr/bin/env bash
# 启动后完整检查：服务、资源、通道、API
set -u
cd "$HOME/super-feiyu" || exit 1

PID=$(pgrep -f 'build/super-feiyu --config' | head -1)
if [ -z "$PID" ]; then
  echo "✗ 服务未运行"
  exit 1
fi

echo "════════ 服务"
echo "  状态: $(systemctl --user is-active super-feiyu)"
echo "  PID:  $PID"
echo "  运行: $(ps -o etime= -p "$PID" | tr -d ' ')"
echo "  线程: $(ls /proc/"$PID"/task | wc -l)"
echo "  RSS:  $(ps -o rss= -p "$PID" | awk '{printf "%.1f MB", $1/1024}')"
echo "  fd:   $(ls /proc/"$PID"/fd | wc -l)"

echo
echo "════════ 空闲 CPU（采样 10 秒）"
A=$(awk '{print $14+$15}' /proc/"$PID"/stat)
sleep 10
B=$(awk '{print $14+$15}' /proc/"$PID"/stat)
awk -v d=$((B-A)) 'BEGIN{printf "  %.1f%%\n", d/100/10*100}'

echo
echo "════════ 加载项"
journalctl --user -u super-feiyu --since '2 minutes ago' --no-pager 2>/dev/null \
  | grep -oE '"event":"(portraits_loaded|meme|persona|tools_ready|listening|started|ready)","[^}]*' \
  | tail -8 | sed 's/^/  /'

echo
echo "════════ QQ 通道"
journalctl --user -u super-feiyu --since '2 minutes ago' --no-pager 2>/dev/null \
  | grep -E 'qq_ws.*(ready|started)' | tail -2 | sed 's/^/  /'

echo
echo "════════ 控制面 API"
TOKEN=$(cat .api_token)
curl -s -m 3 -o /dev/null -w "  healthz: %{http_code}\n" http://127.0.0.1:8765/healthz
echo -n "  人格: "
curl -s -m 3 -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8765/v1/persona/default

echo
echo "════════ 错误统计"
journalctl --user -u super-feiyu --since '2 minutes ago' --no-pager 2>/dev/null \
  | grep -c '"level":"error"' | xargs echo "  错误数:"
