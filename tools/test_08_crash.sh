#!/usr/bin/env bash
# 全量测试 8/9：崩溃恢复与数据完整性
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

PID=$(pgrep -f 'build/super-feiyu --config' | head -1)
echo "════════ 8.1 崩溃前状态"
echo "  PID: $PID"
echo "  运行时长: $(ps -o etime= -p $PID | tr -d ' ')"
echo "  RSS: $(ps -o rss= -p $PID | awk '{printf "%.1f MB", $1/1024}')"

echo
echo "════════ 8.2 kill -9 强杀（模拟崩溃，不给清理机会）"
systemctl --user stop super-feiyu > /dev/null 2>&1
# 先确保是 stop 状态，再手动拉起，然后 kill -9
nohup ./build/super-feiyu --config config.toml > /tmp/crash_test.log 2>&1 &
MANUAL_PID=$!
sleep 5
echo "  手动进程: $MANUAL_PID"
kill -9 "$MANUAL_PID" 2>/dev/null
sleep 1
if kill -0 "$MANUAL_PID" 2>/dev/null; then
  echo "  ⚠ 没杀掉"
else
  echo "  ✓ 已强杀"
fi

echo
echo "════════ 8.3 强杀后数据完整性检查"
echo "  SQLite 完整性:"
python3 - <<'PY'
import sqlite3, os
p = os.path.expanduser("~/super-feiyu/data/memory/index.db")
if not os.path.exists(p):
    print("    (库不存在)"); raise SystemExit
try:
    c = sqlite3.connect(p)
    r = c.execute("PRAGMA integrity_check").fetchone()[0]
    print(f"    integrity_check: {r}")
    n = c.execute("select count(*) from items").fetchone()[0]
    print(f"    items: {n} 行")
except Exception as e:
    print(f"    ✗ 库损坏: {e}")
PY
echo "  WAL/SHM 残留:"
ls -la data/memory/index.db* 2>/dev/null | awk '{printf "    %s  %s\n", $5, $9}'
echo "  persona 文件:"
ls -la data/persona/*.md 2>/dev/null | awk '{printf "    %s  %s\n", $5, $9}'

echo
echo "════════ 8.4 重启恢复"
systemctl --user reset-failed super-feiyu >/dev/null 2>&1
systemctl --user start super-feiyu
sleep 6
echo "  服务状态: $(systemctl --user is-active super-feiyu)"
NEWPID=$(pgrep -f 'build/super-feiyu --config' | head -1)
echo "  新 PID: $NEWPID"
echo "  RSS: $(ps -o rss= -p ${NEWPID:-1} 2>/dev/null | awk '{printf "%.1f MB", $1/1024}')"

echo
echo "════════ 8.5 恢复后功能自检"
journalctl --user -u super-feiyu -n 30 --no-pager 2>/dev/null \
  | grep -E 'portraits_loaded|meme.*loaded|persona.*ready|qq_ws.*ready|error' | tail -6 | sed 's/^/    /'

echo
echo "════════ 8.6 systemd 自动重启验证（杀掉让它自己起）"
systemctl --user show super-feiyu -p Restart -p RestartSec --value 2>/dev/null | sed 's/^/    /'
KILLPID=$(pgrep -f 'build/super-feiyu --config' | head -1)
if [ -n "$KILLPID" ]; then
  kill -9 "$KILLPID" 2>/dev/null
  echo "  已 kill $KILLPID，等 12 秒看是否自动拉起..."
  sleep 12
  AFTER=$(pgrep -f 'build/super-feiyu --config' | head -1)
  if [ -n "$AFTER" ] && [ "$AFTER" != "$KILLPID" ]; then
    echo "  ✓ 自动重启成功，新 PID=$AFTER"
  else
    echo "  ✗ 没有自动重启（PID=$AFTER）"
  fi
  echo "  服务状态: $(systemctl --user is-active super-feiyu)"
fi
