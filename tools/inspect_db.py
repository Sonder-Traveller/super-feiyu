#!/usr/bin/env python3
"""检查记忆索引库里残留了什么。"""
from __future__ import annotations

import sqlite3
from pathlib import Path

db = Path.home() / "super-feiyu/data/memory/index.db"
print(f"db: {db}  exists={db.exists()}")
if not db.exists():
    raise SystemExit(0)

con = sqlite3.connect(db)
tables = [
    r[0]
    for r in con.execute("select name from sqlite_master where type='table'")
]
print("tables:", tables)

for t in tables:
    n = con.execute(f"select count(*) from {t}").fetchone()[0]
    print(f"  {t}: {n} 行")
    if n and t != "items":
        for row in con.execute(f"select * from {t} limit 3"):
            print("   ", row)
    elif n:
        for row in con.execute("select kind, count(*) from items group by kind"):
            print("    kind:", row)
