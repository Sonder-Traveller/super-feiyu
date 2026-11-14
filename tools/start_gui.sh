#!/usr/bin/env bash
# 用 venv 安装 flet + flet-web，然后启动 GUI（Web 模式）
set -euo pipefail
cd "$HOME/super-feiyu" || exit 1

VENV="$HOME/super-feiyu/.venv"

echo "=== 1. 建虚拟环境"
if [ ! -d "$VENV" ]; then
  python3 -m venv "$VENV" 2>&1 | tail -2 || {
    echo "venv 创建失败，尝试安装 python3-venv"
    exit 1
  }
fi
echo "  $VENV"

echo
echo "=== 2. 安装依赖"
"$VENV/bin/pip" install -q --upgrade pip 2>&1 | tail -1
"$VENV/bin/pip" install -q flet flet-web 2>&1 | tail -3
echo "  flet: $("$VENV/bin/python" -c 'import flet; print("ok")' 2>&1)"
echo "  flet_web: $("$VENV/bin/python" -c 'import flet_web; print("ok")' 2>&1)"

echo
echo "=== 3. 停掉旧进程"
pkill -f 'gui/app.py' 2>/dev/null || true
sleep 1

echo "=== 4. 启动 GUI"
FEIYU_HOST=127.0.0.1 FEIYU_PORT=8765 \
  nohup "$VENV/bin/python" gui/app.py --web --web-port 8550 \
  > /tmp/gui.log 2>&1 &
sleep 8

echo "=== 5. 检查"
pgrep -af 'gui/app.py' | head -2 | sed 's/^/  /'
ss -tln 2>/dev/null | grep 8550 | sed 's/^/  /' || echo "  (未监听 8550)"

for ip in 127.0.0.1 100.64.0.5 192.168.1.134; do
  code=$(curl -s -m 4 -o /dev/null -w '%{http_code}' "http://$ip:8550/" 2>/dev/null)
  printf "  %-16s -> %s\n" "$ip" "${code:-失败}"
done

echo
echo "=== 6. 日志"
tail -15 /tmp/gui.log | sed 's/^/  /'
