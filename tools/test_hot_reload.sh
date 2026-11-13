#!/usr/bin/env bash
# 验证人格文件热加载：改文件 → 不重启 → 下条消息生效
set -u
P="$HOME/super-feiyu/data/persona/default.md"

echo "=== 当前人格"
cat "$P" 2>/dev/null || echo "(文件不存在)"
echo

echo "=== 改成猫娘，不重启"
cat > "$P" <<'EOF'
你是一只猫娘，句尾加「喵」，活泼粘人。
EOF
cat "$P"
echo

echo "=== 等一下，让人格生效"
sleep 1

echo "=== 恢复"
cat > "$P" <<'EOF'
You are a helpful assistant.
EOF
echo "已恢复: $(cat "$P")"
