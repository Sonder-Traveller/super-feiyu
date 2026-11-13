#!/usr/bin/env bash
# 在服务器上启动 super-feiyu（后台常驻）。
# 用法: ./run.sh [start|stop|restart|status|log]
set -uo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

PIDFILE="$DIR/logs/feiyu.pid"
LOGFILE="$DIR/logs/live.log"
mkdir -p "$DIR/logs"

start() {
  if [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "[!] 已在运行 (pid $(cat "$PIDFILE"))"
    return 0
  fi
  if [[ ! -f .env.local ]]; then
    echo "[x] 缺少 .env.local" >&2
    return 1
  fi
  set -a
  # shellcheck disable=SC1091
  . ./.env.local
  set +a

  nohup ./super-feiyu --config config.toml >> "$LOGFILE" 2>&1 &
  local pid=$!
  echo "$pid" > "$PIDFILE"
  sleep 6

  if kill -0 "$pid" 2>/dev/null; then
    echo "[✓] 已启动 (pid $pid)"
    grep -E '"event":"(ready|channel_enabled|connected)"' "$LOGFILE" 2>/dev/null | tail -3
  else
    echo "[x] 启动失败，日志尾部：" >&2
    tail -15 "$LOGFILE" >&2
    rm -f "$PIDFILE"
    return 1
  fi
}

stop() {
  if [[ -f "$PIDFILE" ]]; then
    local pid
    pid=$(cat "$PIDFILE")
    kill "$pid" 2>/dev/null && echo "[✓] 已停止 (pid $pid)"
    rm -f "$PIDFILE"
  else
    pkill -f '[s]uper-feiyu --config' 2>/dev/null && echo "[✓] 已停止" || echo "[!] 未在运行"
  fi
}

case "${1:-start}" in
  start)   start ;;
  stop)    stop ;;
  restart) stop; sleep 2; start ;;
  status)
    if [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
      echo "[运行中] pid $(cat "$PIDFILE")"
      grep -E '"event":"(ready|channel_enabled|metrics)"' "$LOGFILE" 2>/dev/null | tail -3
    else
      echo "[未运行]"
    fi
    ;;
  log) tail -f "$LOGFILE" ;;
  *) echo "用法: $0 {start|stop|restart|status|log}" ;;
esac
