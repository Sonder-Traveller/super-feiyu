#!/usr/bin/env bash
# 修正 [capability.chat]：强制 deepseek + 思考强度 high
#
# 背景：config.toml 被 .gitignore 排除，不在版本控制里。
# 之前改好的配置在某次 deploy_wsl.sh 的 rsync 中被 Windows 侧的旧版覆盖了。
# 这个脚本把正确的值写回去。
set -euo pipefail
cd "$HOME/super-feiyu" || exit 1

CFG=config.toml
cp "$CFG" "$CFG.bak-$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
import re
from pathlib import Path

p = Path.home() / "super-feiyu/config.toml"
text = p.read_text(encoding="utf-8")

new_section = '''[capability.chat]
provider = "openai"
base_url = "http://127.0.0.1:18091/v1"
api_key = "bridge-local"   # 桥接层不校验 key，非空即可

# 模型名走桥接层的抽象名。实测（2026-11-13，各采样 6 次）：
#   fast-model     → deepseek-v4.1-flash   6/6 稳定
#   deep-model     → glm-5.3               6/6（不是 deepseek）
#   balanced-model → glm-5.3
#   auto           → glm-5.3-flash
# 要固定用 DeepSeek，必须写 fast-model。
model = "fast-model"

# 思考强度：low / medium / high / xhigh / max。
# deepseek-v4.1-flash 声明支持 low / high / max。
# 不写这一项 = 用上游默认档（实测默认档几乎不思考）。
reasoning_effort = "high"

# max_tokens / temperature 不设，让上游用自己的默认值。
timeout_ms = 60000
'''

pattern = re.compile(r"\[capability\.chat\][^\[]*", re.MULTILINE)
if not pattern.search(text):
    raise SystemExit("找不到 [capability.chat] 段")

text = pattern.sub(new_section + "\n", text, count=1)
p.write_text(text, encoding="utf-8")
print("已更新 [capability.chat]")
PY

echo
echo "=== 改后:"
sed -n '/^\[capability.chat\]/,/^timeout_ms/p' "$CFG"

echo
echo "=== 重启"
systemctl --user restart super-feiyu
sleep 6
echo "  状态: $(systemctl --user is-active super-feiyu)"

echo
echo "=== 验证真实路由（打一次桥接层）"
curl -s -m 60 http://127.0.0.1:18091/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer bridge-local' \
  -d '{"model":"fast-model","messages":[{"role":"user","content":"只回答：好"}],"reasoning_effort":"high","stream":true}' \
  | python3 -c 'import sys,json; d=json.load(sys.stdin); print(f"  实际模型: {d.get(\"model\")}"); print(f"  思考token: {(d.get(\"usage\") or {}).get(\"completion_tokens_details\",{}).get(\"reasoning_tokens\")}")' 2>/dev/null || echo "  (验证失败，稍后重试)"
