#!/usr/bin/env python3
"""探测 QQ 官方机器人凭据是否生效。

用法:
    python3 tools/qq_probe.py            # 从 .env.local 读凭据
    python3 tools/qq_probe.py --retries 5

检查项:
    1. 用 appId + clientSecret 换 access_token
    2. 用 token 查机器人自身的频道列表（验证权限是否开通）

退出码: 0 = 凭据可用；1 = 不可用
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

TOKEN_URL = "https://bots.qq.com/app/getAppAccessToken"
API_BASE = "https://api.sgroup.qq.com"


def load_env(path: Path) -> dict[str, str]:
    """读取 .env.local 形式的 KEY=VALUE 文件。"""
    out: dict[str, str] = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        # 剥 CR：Windows 写的 .env 在 WSL/跨平台读取时会带 \r，
        # 拼进密钥会导致 100007 appid invalid。
        out[k.strip()] = v.strip().rstrip("\r\n")
    return out


def get_token(app_id: str, secret: str) -> dict:
    body = json.dumps({"appId": app_id, "clientSecret": secret}).encode()
    req = urllib.request.Request(
        TOKEN_URL,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read().decode("utf-8"))


def api_get(path: str, token: str) -> tuple[int, str]:
    req = urllib.request.Request(
        f"{API_BASE}{path}",
        headers={"Authorization": f"QQBot {token}", "Content-Type": "application/json"},
        method="GET",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, r.read().decode("utf-8")[:400]
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8")[:400]


def main() -> int:
    ap = argparse.ArgumentParser(description="探测 QQ 机器人凭据")
    ap.add_argument("--env", default=".env.local")
    ap.add_argument("--retries", type=int, default=3)
    ap.add_argument("--interval", type=float, default=10.0)
    args = ap.parse_args()

    env = load_env(Path(args.env))
    app_id = env.get("QQ_APP_ID", "")
    secret = env.get("QQ_APP_SECRET", "")
    if not app_id or not secret:
        print(f"[x] {args.env} 里缺少 QQ_APP_ID / QQ_APP_SECRET", file=sys.stderr)
        return 1

    print(f"AppID: {app_id}")
    print(f"Secret: {secret[:6]}…{secret[-4:]} (len={len(secret)})")
    print()

    token = ""
    for i in range(args.retries):
        try:
            d = get_token(app_id, secret)
        except Exception as e:
            print(f"  第{i + 1}次请求失败: {type(e).__name__}: {e}")
            time.sleep(args.interval)
            continue

        token = d.get("access_token", "")
        if token:
            print(f"[✓] 拿到 access_token (expires_in={d.get('expires_in')}s)")
            break
        print(f"  第{i + 1}次: code={d.get('code')} msg={d.get('message')}")
        if i < args.retries - 1:
            time.sleep(args.interval)

    if not token:
        print("\n[x] 凭据无效。可能原因：")
        print("    - 机器人刚创建，服务端还没生效（等 1-2 分钟重试）")
        print("    - AppSecret 已被重置，需要重新扫码绑定")
        print("    - 扫码选的不是「创建新机器人」而是别的流程")
        return 1

    print("\n验证 API 权限:")
    for path, label in [
        ("/users/@me", "机器人自身信息"),
        ("/users/@me/guilds", "已加入的频道"),
    ]:
        code, body = api_get(path, token)
        ok = "✓" if code == 200 else "✗"
        print(f"  [{ok}] {label}: HTTP {code}")
        if code != 200:
            print(f"       {body[:200]}")

    print("\n[✓] 凭据可用")
    return 0


if __name__ == "__main__":
    sys.exit(main())
