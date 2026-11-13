#!/usr/bin/env bash
# 核对授权状态：找所有 license/readme 里的授权表述
set -u
cd /tmp/asr || exit 1

echo "════════ gmskywalker (无 LICENSE 文件，查 README)"
ls gmskywalker_deepseek-fat-fish-codex-pet/
echo "--- README 授权相关行:"
grep -inE "licen|许可|授权|copyright|版权|商用|commercial|CC BY|MIT" \
  gmskywalker_deepseek-fat-fish-codex-pet/README*.md 2>/dev/null | head -20

echo
echo "════════ bloodarea 分离授权细则"
sed -n '1,80p' bloodarea_dsh-pets/LICENSE

echo
echo "════════ Neko3000 资产授权范围"
grep -inE "CC BY-NC-SA|assets|角色|character|素材|非商业|non-?commercial" \
  Neko3000_deepseek-whalechan/README.md 2>/dev/null | head -25
