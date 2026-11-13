#!/usr/bin/env python3
"""检查解密出的 AppSecret 字节是否干净。"""
import re
from pathlib import Path


def load_env(p: Path) -> dict[str, str]:
    out = {}
    for line in p.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


env = load_env(Path(".env.local"))
secret = env["QQ_APP_SECRET"]

print(f"repr      : {secret!r}")
print(f"len       : {len(secret)}")
print(f"bytes len : {len(secret.encode('utf-8'))}")
print(f"all ASCII : {secret.isascii()}")
print(f"alnum only: {bool(re.fullmatch(r'[A-Za-z0-9]+', secret))}")
print(f"char set  : {sorted(set(secret))}")
print(f"has space : {' ' in secret}")
print(f"has quote : {chr(34) in secret or chr(39) in secret}")
