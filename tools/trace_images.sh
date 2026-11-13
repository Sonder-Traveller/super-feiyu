#!/usr/bin/env bash
# 排查图片发送链路
set -u
echo "=== 最近 40 条非心跳日志"
journalctl --user -u super-feiyu -n 300 --no-pager 2>/dev/null | \
  grep -vE 'heartbeat|"event":"ping"' | tail -40
