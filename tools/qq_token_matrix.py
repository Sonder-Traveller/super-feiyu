#!/usr/bin/env python3
"""排查 100016：试不同的请求格式，看服务端是否接受。"""
import json
import urllib.error
import urllib.request
from pathlib import Path

TOKEN_URL = "https://bots.qq.com/app/getAppAccessToken"


def load_env(p: Path) -> dict[str, str]:
    out = {}
    for line in p.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def try_post(label: str, payload: dict, ua: str) -> None:
    body = json.dumps(payload).encode()
    req = urllib.request.Request(
        TOKEN_URL,
        data=body,
        headers={
            "Content-Type": "application/json",
            "User-Agent": ua,
            "Content-Length": str(len(body)),
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            d = json.loads(r.read().decode())
        if d.get("access_token"):
            print(f"  [✓] {label}: 成功 token={d['access_token'][:10]}…")
        else:
            print(f"  [✗] {label}: code={d.get('code')} msg={d.get('message')}")
    except urllib.error.HTTPError as e:
        print(f"  [✗] {label}: HTTP {e.code} {e.read().decode()[:120]}")
    except Exception as e:
        print(f"  [✗] {label}: {type(e).__name__}: {e}")


env = load_env(Path(".env.local"))
app_id = env["QQ_APP_ID"]
secret = env["QQ_APP_SECRET"]

print(f"appId={app_id} secret_len={len(secret)}")
print()

# 1. 字符串 appId（我们当前用的）
try_post("appId 字符串", {"appId": app_id, "clientSecret": secret},
         "super-feiyu/1.0")

# 2. 数字 appId
try_post("appId 数字", {"appId": int(app_id), "clientSecret": secret},
         "super-feiyu/1.0")

# 3. 官方 SDK 的 User-Agent
try_post("官方 UA", {"appId": app_id, "clientSecret": secret},
         "qqbot-nodejs/1.2.0")

# 4. 故意用错的 secret —— 看错误码是否相同
try_post("错误 secret（对照）", {"appId": app_id, "clientSecret": "x" * 32},
         "super-feiyu/1.0")

# 5. 故意用错的 appId —— 看错误码是否相同
try_post("错误 appId（对照）", {"appId": "1234567890", "clientSecret": secret},
         "super-feiyu/1.0")
