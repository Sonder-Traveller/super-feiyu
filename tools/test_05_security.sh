#!/usr/bin/env bash
# 全量测试 5/9：安全
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

echo "════════ 5.1 输入注入检测（单元层）"
grep -A6 'guard: injection detection' /tmp/t2_test_all.log 2>/dev/null | head -10

echo
echo "════════ 5.2 超长输入"
grep -A5 'guard: length limit' /tmp/t2_test_all.log 2>/dev/null | head -8

echo
echo "════════ 5.3 输出脱敏"
grep -A6 'guard: output sanitize' /tmp/t2_test_all.log 2>/dev/null | head -10

echo
echo "════════ 5.4 限流"
grep -A6 'guard: rate limiter' /tmp/t2_test_all.log 2>/dev/null | head -10

echo
echo "════════ 5.5 人格存储路径穿越"
grep -A2 'path traversal' /tmp/t2_test_all.log 2>/dev/null | head -4

echo
echo "════════ 5.6 API 鉴权（单元层）"
grep -E 'token rejected|token accepted|healthz' /tmp/t2_test_all.log 2>/dev/null | head -6

echo
echo "════════ 5.7 敏感信息是否泄露在日志里"
echo "  .env.local 变量值是否出现在日志:"
for k in SILICONFLOW_API_KEY QQ_APP_SECRET QQ_APP_ID; do
  v=$(grep -E "^$k=" .env.local 2>/dev/null | cut -d= -f2- | head -1)
  if [ -n "$v" ] && [ ${#v} -gt 6 ]; then
    hit=$(journalctl --user -u super-feiyu --since '1 hour ago' --no-pager 2>/dev/null | grep -c "$v" || true)
    echo "    $k: 出现 $hit 次"
  else
    echo "    $k: (空或未配置)"
  fi
done

echo
echo "════════ 5.8 日志里是否含完整 token"
journalctl --user -u super-feiyu --since '1 hour ago' --no-pager 2>/dev/null \
  | grep -oE 'access_token[^,]{0,20}' | head -3 || echo "    (无)"

echo
echo "════════ 5.9 文件权限"
echo "  .env.local: $(stat -c '%a %U' .env.local 2>/dev/null)"
echo "  config.toml: $(stat -c '%a %U' config.toml 2>/dev/null)"
echo "  data/persona: $(stat -c '%a' data/persona 2>/dev/null)"
echo "  data/memory: $(stat -c '%a' data/memory 2>/dev/null)"
