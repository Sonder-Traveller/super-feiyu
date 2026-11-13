#!/usr/bin/env python3
"""生成「加机器人进群 / 打开对话」的二维码。

QQ 官方机器人不是靠搜索找到的，必须用分享链接扫码添加。
本工具从 /users/@me 取 share_url，再渲染成二维码。

用法:
    python3 tools/qq_qr.py                    # 生成 share 二维码
    python3 tools/qq_qr.py --out bot.png
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from qq_probe import api_get, get_token, load_env  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description="生成机器人添加二维码")
    ap.add_argument("--env", default=".env.local")
    ap.add_argument("--out", default=".qq_bot_qr.png")
    args = ap.parse_args()

    env = load_env(Path(args.env))
    app_id = env.get("QQ_APP_ID", "")
    secret = env.get("QQ_APP_SECRET", "")
    if not app_id or not secret:
        print(f"[x] {args.env} 缺少 QQ_APP_ID / QQ_APP_SECRET", file=sys.stderr)
        return 1

    d = get_token(app_id, secret)
    token = d.get("access_token", "")
    if not token:
        print(f"[x] 取 token 失败: {d}", file=sys.stderr)
        return 1

    code, body = api_get("/users/@me", token)
    if code != 200:
        print(f"[x] /users/@me 失败: HTTP {code} {body[:200]}", file=sys.stderr)
        return 1

    me = json.loads(body)
    name = me.get("username", "?")
    share_url = me.get("share_url", "")

    print(f"机器人: {name}")
    print(f"  id      : {me.get('id')}")
    print(f"  share   : {share_url}")
    print()

    if not share_url:
        print("[x] 没有 share_url，无法生成二维码", file=sys.stderr)
        return 1

    try:
        import qrcode
    except ImportError:
        print("[x] 需要 qrcode: pip install qrcode pillow", file=sys.stderr)
        return 1

    qr = qrcode.QRCode(border=2, box_size=10)
    qr.add_data(share_url)
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white")
    img.save(args.out)
    print(f"[✓] 二维码已保存: {args.out}")
    print()
    print("扫码用途：把这个机器人加到 QQ 群，或打开与它的对话。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
