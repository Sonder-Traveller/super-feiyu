#!/usr/bin/env bash
# 补充测试：QQ 通道真实链路 + 消息流水线
set -uo pipefail
cd "$HOME/super-feiyu" || exit 1

echo "════════ A. 通道能力声明"
grep -E 'max_replies|support' src/fish.channels.cppm | grep -E '=\s*(true|[0-9]+)' | head -12 | sed 's/^/    /'

echo
echo "════════ B. 被动回复配额（平台硬约束）"
echo "  单聊 4 / 群 5 —— 源码里的值:"
grep -n 'max_replies_per_message' src/fish.channels.cppm | sed 's/^/    /'

echo
echo "════════ C. 运行期消息处理统计"
journalctl --user -u super-feiyu --since '2 hours ago' --no-pager 2>/dev/null \
  | grep -oE '"event":"metrics","polled":[0-9]+,"handled":[0-9]+,"errors":[0-9]+' \
  | tail -3 | sed 's/^/    /'

echo
echo "════════ D. 完整流水线事件覆盖情况"
echo "  日志里出现过的 pipeline 事件:"
journalctl --user -u super-feiyu --since '3 hours ago' --no-pager 2>/dev/null \
  | grep -oE '"module":"pipeline","event":"[a-z_]+"' \
  | sed 's/.*"event":"//;s/"//' | sort | uniq -c | sed 's/^/    /'

echo
echo "════════ E. 工具调用是否被触发过"
journalctl --user -u super-feiyu --since '3 hours ago' --no-pager 2>/dev/null \
  | grep -E 'tool_ran|tool"' | tail -3 | sed 's/^/    /'
echo "  (空 = 至今没有真实消息触发过工具调用)"

echo
echo "════════ F. 图片/语音发送记录"
journalctl --user -u super-feiyu --since '3 hours ago' --no-pager 2>/dev/null \
  | grep -E 'photo_attached|voice_attached|upload_ok' | tail -5 | sed 's/^/    /'
echo "  (空 = 至今没有真实发过图/语音)"

echo
echo "════════ G. 检查潜在的死代码"
echo "  extract_voice_marker 是否被调用:"
grep -rn 'extract_voice_marker' src/ | sed 's/^/    /'
echo "  extract_photo_markers 是否被调用:"
grep -rn 'extract_photo_markers' src/ | sed 's/^/    /'
