#!/usr/bin/env bash
# 全量测试 7/9：记忆系统
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

echo "════════ 7.1 当前记忆状态"
echo "  notes 文件: $(find data/memory/notes -type f 2>/dev/null | wc -l)"
echo "  index.db:   $(stat -c%s data/memory/index.db 2>/dev/null || echo 不存在) 字节"
echo "  media 缓存: $(find data/memory/media -type f 2>/dev/null | wc -l) 个"

echo
echo "════════ 7.2 index.db 内容"
python3 - <<'PY'
import sqlite3, os
p = os.path.expanduser("~/super-feiyu/data/memory/index.db")
if not os.path.exists(p):
    print("  (不存在)"); raise SystemExit
c = sqlite3.connect(p)
tabs = [r[0] for r in c.execute("select name from sqlite_master where type='table'")]
print(f"  表: {tabs}")
for t in tabs:
    n = c.execute(f"select count(*) from {t}").fetchone()[0]
    print(f"  {t}: {n} 行")
PY

echo
echo "════════ 7.3 单元层记忆测试明细"
grep -A40 '=== memory tests ===' /tmp/t2_test_memory.log 2>/dev/null | grep -E '^\[PASS\]|^\[FAIL\]' | head -35

echo
echo "════════ 7.4 记忆相关测试主题（test_all）"
grep -A3 -E '=== (working memory|notes store|index store|consolidation|index rebuild|gather)' \
  /tmp/t2_test_all.log 2>/dev/null | grep -E '^\[PASS\]|^===' | head -40

echo
echo "════════ 7.5 索引重建能力（删了能否从文件恢复）"
grep -A5 'index rebuild from notes' /tmp/t2_test_all.log 2>/dev/null | head -8

echo
echo "════════ 7.6 会话隔离"
grep -A4 'session: key derivation' /tmp/t2_test_all.log 2>/dev/null | head -8
echo "  --- 实际目录结构:"
find data/memory/notes -type d 2>/dev/null | head -10 | sed 's/^/    /'

echo
echo "════════ 7.7 token 预算与压缩"
grep -A8 'token 预算压缩' /tmp/t2_test_lifecycle.log 2>/dev/null | head -10
