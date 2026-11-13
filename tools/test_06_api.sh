#!/usr/bin/env bash
# 全量测试 6/9：控制面 API 深度测试
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

UNIT="$HOME/.config/systemd/user/super-feiyu.service"
CFG=config.toml
TOKEN="deep-test-$$"
PORT=18767

cp "$CFG" /tmp/c.bak; cp "$UNIT" /tmp/u.bak
cleanup() {
  cp /tmp/c.bak "$CFG"; cp /tmp/u.bak "$UNIT"
  systemctl --user daemon-reload >/dev/null 2>&1
  systemctl --user restart super-feiyu >/dev/null 2>&1
  echo "[已恢复]"
}
trap cleanup EXIT

if grep -q '^\[api\]' "$CFG"; then
  sed -i "s/^port = .*/port = $PORT/" "$CFG"
else
  printf '\n[api]\nport = %s\nbind = "127.0.0.1"\nauth = true\n' "$PORT" >> "$CFG"
fi
if grep -q '^Environment=FEIYU_API_TOKEN=' "$UNIT"; then
  sed -i "s|^Environment=FEIYU_API_TOKEN=.*|Environment=FEIYU_API_TOKEN=$TOKEN|" "$UNIT"
else
  sed -i "/^\[Service\]/a Environment=FEIYU_API_TOKEN=$TOKEN" "$UNIT"
fi

systemctl --user daemon-reload
systemctl --user restart super-feiyu
sleep 6
[ "$(systemctl --user is-active super-feiyu)" = "active" ] || { echo "服务起不来"; exit 1; }

B="http://127.0.0.1:$PORT"
H="Authorization: Bearer $TOKEN"
code() { curl -s -m 5 -o /dev/null -w '%{http_code}' "$@"; }

echo "════════ 6.1 状态码矩阵"
printf "  GET  /healthz            无token   -> %s (期望200)\n" "$(code $B/healthz)"
printf "  GET  /v1/persona/default 无token   -> %s (期望401)\n" "$(code $B/v1/persona/default)"
printf "  GET  /v1/persona/default 有token   -> %s (期望200)\n" "$(code -H "$H" $B/v1/persona/default)"
printf "  GET  /v1/unknown         有token   -> %s (期望404)\n" "$(code -H "$H" $B/v1/unknown)"
printf "  POST /v1/persona/default 有token   -> %s (期望404/405)\n" "$(code -X POST -H "$H" $B/v1/persona/default)"
printf "  GET  /v1/persona/session 缺conv    -> %s (期望400)\n" "$(code -H "$H" $B/v1/persona/session)"
printf "  PUT  /v1/persona/default 空body    -> %s (期望200,空人格)\n" "$(code -X PUT -H "$H" -d '{}' $B/v1/persona/default)"

echo
echo "════════ 6.2 畸形输入"
printf "  坏 JSON           -> %s\n" "$(code -X PUT -H "$H" --data-binary 'not json' $B/v1/persona/default)"
printf "  缺 persona 字段   -> %s\n" "$(code -X PUT -H "$H" --data-binary '{"other":"x"}' $B/v1/persona/default)"
printf "  非法方法 PATCH    -> %s\n" "$(code -X PATCH -H "$H" $B/v1/persona/default)"

# 超大 body
head -c 2000000 /dev/zero | tr '\0' 'a' > /tmp/big.bin
printf "  超长 body(2MB)    -> %s\n" "$(code -X PUT -H "$H" --data-binary @/tmp/big.bin $B/v1/persona/default)"
rm -f /tmp/big.bin

echo
echo "════════ 6.3 并发 10 个 GET"
start=$(date +%s%N)
for i in $(seq 1 10); do
  curl -s -m 5 -o /dev/null -H "$H" $B/v1/persona/default &
done
wait
end=$(date +%s%N)
echo "  10 并发耗时: $(( (end-start)/1000000 )) ms"

echo
echo "════════ 6.4 特殊字符人格（引号/反斜杠/换行）"
curl -s -X PUT -H "$H" -H 'Content-Type: application/json' \
  --data-binary '{"persona":"引号测试\"双引号\" 换行\n第二行 制表\t结束"}' \
  $B/v1/persona/default
echo "  回读: $(curl -s -H "$H" $B/v1/persona/default)"

echo
echo "════════ 6.5 中文 + emoji 往返"
curl -s -o /dev/null -X PUT -H "$H" -H 'Content-Type: application/json' \
  --data-binary '{"persona":"你是鲸鱼娘🐋，说话要短"}' $B/v1/persona/default
echo "  回读: $(curl -s -H "$H" $B/v1/persona/default)"

echo
echo "════════ 6.6 落盘校验"
echo "  default.md 字节: $(stat -c%s data/persona/default.md 2>/dev/null)"
echo "  前 80 字节: $(head -c 80 data/persona/default.md 2>/dev/null | cat -v)"

echo
echo "════════ 6.7 压测后资源"
PID=$(pgrep -f 'build/super-feiyu --config' | head -1)
echo "  RSS:  $(ps -o rss= -p $PID | awk '{printf "%.1f MB", $1/1024}')"
echo "  fd:   $(ls /proc/$PID/fd 2>/dev/null | wc -l)"
echo "  线程: $(ls /proc/$PID/task 2>/dev/null | wc -l)"
echo "  服务: $(systemctl --user is-active super-feiyu)"
