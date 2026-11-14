#!/usr/bin/env bash
# 服务就绪总检
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

echo "════════ 服务"
echo "  systemd: $(systemctl --user is-active super-feiyu)"
PID=$(pgrep -f 'build/super-feiyu --config' | head -1)
echo "  PID: $PID"
echo "  运行时长: $(ps -o etime= -p "$PID" | tr -d ' ')"
echo "  RSS: $(ps -o rss= -p "$PID" | awk '{printf "%.1f MB", $1/1024}')"
echo "  线程: $(ls /proc/$PID/task | wc -l)"
echo "  fd: $(ls /proc/$PID/fd | wc -l)"

echo
echo "════════ 加载项"
journalctl --user -u super-feiyu --since '2 minutes ago' --no-pager 2>/dev/null \
  | grep -oE '"event":"(portraits_loaded|meme|persona|tools_ready|ready|listening|started)"[^}]*' \
  | tail -10 | sed 's/^/    /'

echo
echo "════════ QQ 通道"
journalctl --user -u super-feiyu --since '2 minutes ago' --no-pager 2>/dev/null \
  | grep -E 'qq_ws.*(ready|started)|qq.*token_ok' | tail -3 | sed 's/^/    /'

echo
echo "════════ 控制面 API"
PORT=$(grep -A3 '^\[api\]' config.toml | grep -oE 'port = [0-9]+' | grep -oE '[0-9]+')
echo "  监听: 127.0.0.1:$PORT"
echo "  token 文件: $(ls -la .api_token 2>/dev/null | awk '{print $1, $9}')"
TOKEN=$(cat .api_token 2>/dev/null)
B="http://127.0.0.1:$PORT"
printf "  healthz: %s\n" "$(curl -s -m 5 $B/healthz)"

echo
echo "════════ 当前配置摘要"
echo "  模型: $(grep -A6 '^\[capability.chat\]' config.toml | grep '^model' | head -1)"
echo "  思考强度: $(grep -A6 '^\[capability.chat\]' config.toml | grep 'reasoning_effort' | head -1)"
echo "  人格文件: $(stat -c%s data/persona/default.md 2>/dev/null) 字节"
echo "  立绘: $(find data/memory/portraits -name '*.png' 2>/dev/null | wc -l) 张"
echo "  热梗: $(grep -vcE '^(#|term\t|$)' data/memes/memes.tsv 2>/dev/null) 条"

echo
echo "════════ 最近错误"
journalctl --user -u super-feiyu --since '2 minutes ago' --no-pager 2>/dev/null \
  | grep -c '"level":"error"' | xargs echo "  错误数:"
