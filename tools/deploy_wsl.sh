#!/usr/bin/env bash
# 在 WSL 部署 super-feiyu：同步源码 + 保留素材与配置 + 重启服务。
#
# 与 deploy/install.sh 的区别：那个脚本 rsync 带 --delete 且排除 data/，
# 会删掉服务器上已有的立绘和记忆。这个脚本只同步代码，data/ 一律不动。
set -euo pipefail

WIN_SRC=/mnt/c/Users/Assistant/projects/super-feiyu
DST="$HOME/super-feiyu"

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

[ -d "$WIN_SRC" ] || die "源目录不存在: $WIN_SRC"
[ -d "$DST" ] || die "部署目录不存在: $DST"

# ── 1. 备份 data（只留最近 3 份）──
say "备份 data"
if [ -d "$DST/data" ]; then
  ts=$(date +%Y%m%d-%H%M%S)
  mkdir -p "$HOME/super-feiyu-backup"
  tar czf "$HOME/super-feiyu-backup/data-$ts.tgz" -C "$DST" data
  ls -1t "$HOME/super-feiyu-backup"/data-*.tgz | tail -n +4 | xargs -r rm -f
  echo "    $(du -h "$HOME/super-feiyu-backup/data-$ts.tgz" | cut -f1)  data-$ts.tgz"
fi

# ── 2. 同步源码（不含 data / 密钥 / 构建产物）──
say "同步源码"
rsync -a \
  --exclude '.git/' --exclude 'build/' --exclude 'build-docker/' \
  --exclude 'build_pcm/' --exclude 'build_obj/' \
  --exclude 'data/' --exclude '.env' --exclude '.env.local' \
  --exclude '*.o' --exclude '*.pcm' --exclude '*.log' \
  --exclude '__pycache__/' \
  "$WIN_SRC"/ "$DST"/

# ── 3. 确保素材就位（data 被备份后仍在，这里只补缺失的）──
say "检查素材"
if [ ! -f "$DST/data/memes/memes.tsv" ]; then
  mkdir -p "$DST/data/memes"
  cp "$WIN_SRC/data/memes/memes.tsv" "$DST/data/memes/" 2>/dev/null || true
  echo "    补了 memes.tsv"
fi
n_portrait=$(find "$DST/data/memory/portraits" -name '*.png' 2>/dev/null | wc -l)
echo "    立绘 $n_portrait 张"

# ── 4. 构建 ──
say "构建"
cd "$DST"
./build.sh 2>&1 | tail -8

# ── 5. 重启服务 ──
say "重启服务"
systemctl --user daemon-reload
systemctl --user reset-failed super-feiyu 2>/dev/null || true
systemctl --user restart super-feiyu

sleep 8
if systemctl --user is-active --quiet super-feiyu; then
  echo "    状态: 运行中"
else
  echo "    状态: 失败"
  journalctl --user -u super-feiyu -n 20 --no-pager
  exit 1
fi

# ── 6. 自检 ──
say "启动日志"
journalctl --user -u super-feiyu -n 40 --no-pager | \
  grep -E 'portraits_loaded|meme|started|ready|error' | tail -10

say "内存占用"
ps -o rss= -C super-feiyu 2>/dev/null | awk '{printf "    %.1f MB\n", $1/1024}'
