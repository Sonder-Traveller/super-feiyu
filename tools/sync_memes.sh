#!/usr/bin/env bash
# 同步 memes.tsv 到 WSL 并校验列数
set -u
SRC=/mnt/c/Users/Assistant/projects/super-feiyu/data/memes/memes.tsv
DST="$HOME/super-feiyu/data/memes/memes.tsv"

mkdir -p "$HOME/super-feiyu/data/memes"
cp "$SRC" "$DST"

echo "总行数: $(wc -l < "$DST")"
echo "数据行（非注释非表头）: $(grep -vcE '^(#|term\t)' "$DST")"

echo "--- 列数不是 6 或 7 的行:"
awk -F'\t' '!/^#/ && NF>1 && NF!=6 && NF!=7 {print "  行 " NR ": NF=" NF}' "$DST"

echo "--- 空 term 的行:"
awk -F'\t' '!/^#/ && NF>1 && $1=="" {print "  行 " NR}' "$DST"
