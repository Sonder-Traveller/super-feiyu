#!/usr/bin/env bash
# 加载 .env.local 后启动，验证立绘加载
set -u
cd "$HOME/super-feiyu" || exit 1

pkill -f 'build/super-feiyu' 2>/dev/null
sleep 1

if [ ! -f .env.local ]; then
  echo "缺少 .env.local，无法启动"
  exit 1
fi

set -a
# shellcheck disable=SC1091
. ./.env.local
set +a

./build/super-feiyu --config config.toml > /tmp/run.log 2>&1 &
PID=$!
echo "pid=$PID"
sleep 10

if kill -0 "$PID" 2>/dev/null; then
  echo "状态: 运行中"
else
  echo "状态: 已退出"
fi

echo "--- 立绘/图片/热梗:"
grep -iE 'portrait|image|meme|memes' /tmp/run.log | head -15
echo "--- 错误与警告:"
grep -iE '"level":"(error|warn)"' /tmp/run.log | head -10
echo "--- 末尾 5 行:"
tail -5 /tmp/run.log
