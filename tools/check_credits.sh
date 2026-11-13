#!/usr/bin/env bash
# 校验 CREDITS.md 里提到的文件名是否都真实存在
set -u
cd "$HOME/super-feiyu" || exit 1

D=data/memory/portraits
echo "--- CREDITS.md 提到但文件不存在:"
grep -oE '`[a-z0-9-]+\.png`' /mnt/c/Users/Assistant/projects/super-feiyu/CREDITS.md \
  | tr -d '`' | sort -u | while read -r f; do
      [ -f "$D/$f" ] || echo "  $f"
    done

echo "--- 文件存在但 CREDITS.md 没提:"
for f in "$D"/*.png; do
  b=$(basename "$f")
  grep -qF "$b" /mnt/c/Users/Assistant/projects/super-feiyu/CREDITS.md || echo "  $b"
done
