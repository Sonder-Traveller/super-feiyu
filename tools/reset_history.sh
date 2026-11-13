#!/usr/bin/env bash
# 清空对话历史，保留素材（立绘、热梗、探测缓存）。
#
# 清掉：
#   data/memory/notes/      对话原文、事实、情景、特质
#   data/memory/index.db    向量索引（可由 notes 重建）
#   data/memory/media/      收到的图片缓存
# 保留：
#   data/memory/portraits/  形象库（素材，不是历史）
#   data/memes/             热梗库（素材，不是历史）
#   data/probe/             能力探测缓存（不是历史）
set -euo pipefail

cd "$HOME/super-feiyu" || exit 1

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

say "清空前统计"
for d in data/memory/notes data/memory/media; do
  n=$(find "$d" -type f 2>/dev/null | wc -l)
  echo "    $d: $n 个文件"
done
[ -f data/memory/index.db ] && echo "    index.db: $(du -h data/memory/index.db | cut -f1)"

say "停止服务"
systemctl --user stop super-feiyu
sleep 2

say "清空历史"
rm -rf data/memory/notes data/memory/media data/memory/index.db
mkdir -p data/memory/notes

say "保留的素材"
echo "    立绘: $(find data/memory/portraits -name '*.png' 2>/dev/null | wc -l) 张"
echo "    热梗: $(grep -vcE '^(#|term\t|$)' data/memes/memes.tsv 2>/dev/null || echo 0) 条"
echo "    探测缓存: $(ls data/probe/*.json 2>/dev/null | wc -l) 个"

say "启动服务"
systemctl --user reset-failed super-feiyu 2>/dev/null || true
systemctl --user start super-feiyu
sleep 8

if systemctl --user is-active --quiet super-feiyu; then
  echo "    状态: 运行中"
else
  echo "    状态: 失败"
  journalctl --user -u super-feiyu -n 15 --no-pager
  exit 1
fi

say "清空后确认"
journalctl --user -u super-feiyu -n 30 --no-pager | \
  grep -E 'portraits_loaded|meme.*loaded|ready' | tail -5
echo "    notes 文件数: $(find data/memory/notes -type f | wc -l)"
echo "    index.db: $([ -f data/memory/index.db ] && echo 已重建 || echo 未生成)"
