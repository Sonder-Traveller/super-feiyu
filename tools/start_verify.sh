#!/usr/bin/env bash
# 前台启动，把输出写日志，便于确认立绘加载
set -u
cd "$HOME/super-feiyu" || exit 1

pkill -f 'build/super-feiyu' 2>/dev/null
sleep 1

./build/super-feiyu --config config.toml > /tmp/run.log 2>&1 &
PID=$!
echo "pid=$PID"
sleep 8

echo "--- 存活检查:"
if kill -0 "$PID" 2>/dev/null; then
  echo "  运行中"
else
  echo "  已退出"
fi

echo "--- 日志行数: $(wc -l < /tmp/run.log)"
echo "--- 立绘/图片相关:"
grep -iE 'portrait|image|meme' /tmp/run.log | head -20
echo "--- 错误:"
grep -iE '"level":"(error|warn)"' /tmp/run.log | head -10
echo "--- 末尾:"
tail -5 /tmp/run.log
