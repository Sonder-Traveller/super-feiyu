#!/usr/bin/env bash
set -u
cd "$HOME/super-feiyu" || exit 1
echo "=== flet.padding 成员"
.venv/bin/python -c "import flet as ft; print([x for x in dir(ft.padding) if not x.startswith('_')])"

echo
echo "=== 常见构造尝试"
.venv/bin/python - <<'PY'
import flet as ft
for expr in ["ft.padding.only(top=6)", "ft.padding.only(6,0,0,0)",
             "ft.Padding.only(top=6)", "ft.Padding(6,0,0,0)",
             "ft.padding.all(6)", "ft.Padding.all(6)",
             "ft.padding.symmetric(horizontal=10, vertical=10)"]:
    try:
        v = eval(expr)
        print(f"  OK   {expr}  -> {v}")
    except Exception as e:
        print(f"  FAIL {expr}  -> {type(e).__name__}: {e}")
PY
