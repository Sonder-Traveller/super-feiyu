#!/usr/bin/env bash
# 全量测试 9/9：文档一致性 + QQ 通道现状
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

echo "════════ 9.1 README 声称 vs 实际"
echo "  README 说模块数:"
grep -oE '[0-9]+ 个 C\+\+20 模块' README.md | head -1
echo "  实际模块数: $(ls src/*.cppm | wc -l)"

echo "  README 说内存:"
grep -oE '15–30 MB|22\.[0-9] MB' README.md | head -2
PID=$(pgrep -f 'build/super-feiyu --config' | head -1)
echo "  实际: $(ps -o rss= -p $PID 2>/dev/null | awk '{printf "%.1f MB", $1/1024}')"

echo "  README 说测试数:"
grep -oE '[0-9]+ 项断言' README.md | head -1
echo "  实际（现跑，不手抄）:"
./tools/count_pass.sh 2>/dev/null | sed 's/^/    /'

echo
echo "════════ 9.2 文档里提到的文件是否存在"
for f in $(grep -oE '\[([^]]+)\]\(([^)]+\.md)\)' README.md 2>/dev/null | grep -oE '\([^)]+\.md\)' | tr -d '()' | sort -u); do
  if [ -f "$f" ]; then
    echo "  ✓ $f"
  else
    echo "  ✗ $f 不存在"
  fi
done

echo
echo "════════ 9.3 persona 目录状态"
ls -la persona/ 2>/dev/null | sed 's/^/    /'
echo "  README/docs 是否还引用已删除的 rules.md:"
grep -rl 'rules\.md' README.md docs/ 2>/dev/null | sed 's/^/    ⚠ /' || echo "    (无)"

echo
echo "════════ 9.4 通道配置现状"
grep -E '^\[channel|^enabled' config.toml | sed 's/^/    /'

echo
echo "════════ 9.5 QQ 通道连通性"
journalctl --user -u super-feiyu --since '5 minutes ago' --no-pager 2>/dev/null \
  | grep -E 'qq_ws|qq.*token' | tail -3 | sed 's/^/    /'

echo
echo "════════ 9.6 能力注册现状"
journalctl --user -u super-feiyu --since '5 minutes ago' --no-pager 2>/dev/null \
  | grep 'tools_ready' | tail -1 | sed 's/^/    /'

echo
echo "════════ 9.7 WORKLOG / 其他文档是否过时"
for f in WORKLOG.md docs/PROMPTS.md docs/API.md docs/PERSONA_REFS.md; do
  [ -f "$f" ] && echo "  $f: $(wc -l < $f) 行, 最后修改 $(stat -c%y "$f" | cut -d. -f1)"
done
