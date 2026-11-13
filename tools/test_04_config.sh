#!/usr/bin/env bash
# 全量测试 4/9：配置健壮性
# 每种坏配置都应该「明确拒绝」，不能静默跑起来
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

BIN=./build/super-feiyu
TMP=/tmp/cfgtest
rm -rf "$TMP"; mkdir -p "$TMP"

run_case() {
  local name="$1"; shift
  local desc="$1"; shift
  echo "── $name"
  echo "   $desc"
  timeout 15 "$@" > "$TMP/out.log" 2>&1
  local rc=$?
  if [ $rc -eq 124 ]; then
    echo "   ⚠ 超时（可能卡住）"
    return
  fi
  echo "   退出码: $rc"
  grep -oE '"event":"[a-z_]+"' "$TMP/out.log" | sort -u | head -5 | sed 's/^/     /'
  grep -oE '"problem":"[^"]+"' "$TMP/out.log" | head -3 | sed 's/^/     /'
  echo
}

echo "════════ 4.1 配置文件不存在"
run_case "缺失配置" "应该报错退出" $BIN --config "$TMP/nonexistent.toml"

echo "════════ 4.2 空配置文件"
: > "$TMP/empty.toml"
run_case "空配置" "应该拒绝（没有通道/没有 key）" $BIN --config "$TMP/empty.toml"

echo "════════ 4.3 语法错误"
printf 'this is not toml\n[broken\n' > "$TMP/bad.toml"
run_case "语法错误" "应该报错" $BIN --config "$TMP/bad.toml"

echo "════════ 4.4 非法数值"
cat > "$TMP/badnum.toml" <<'EOF'
[channel.qq_official]
enabled = true
app_id = "x"
app_secret = "y"
[memory]
model_context_tokens = "not_a_number"
EOF
run_case "非法数值" "应该拒绝或回退到默认" $BIN --config "$TMP/badnum.toml"

echo "════════ 4.5 未定义环境变量"
cat > "$TMP/missingenv.toml" <<'EOF'
[channel.qq_official]
enabled = true
app_id = "${THIS_VAR_DOES_NOT_EXIST}"
app_secret = "${ALSO_MISSING}"
EOF
run_case "缺失环境变量" "应该报 app_id 为空" $BIN --config "$TMP/missingenv.toml"

echo "════════ 4.6 非法端口"
cat > "$TMP/badport.toml" <<'EOF'
[api]
port = 999999
bind = "not-an-ip"
auth = false
EOF
run_case "非法端口/IP" "应该拒绝启动 API" $BIN --config "$TMP/badport.toml"

echo "════════ 4.7 自检模式"
run_case "self-test" "应能完成自检" $BIN --self-test

echo "════════ 4.8 --help / 无参"
timeout 5 $BIN --help > "$TMP/help.log" 2>&1
echo "   --help 退出码: $?"
head -3 "$TMP/help.log" | sed 's/^/     /'

rm -rf "$TMP"
