#!/usr/bin/env python3
"""验证 QQ 官方富媒体的三种上传方式，找出不需要公网端口的那种。

背景：
  之前用 multipart file=@ 上传，报 40093007「富媒体文件下载失败」。
  查官方文档发现正确字段是 JSON 里的 file_data（二进制直传），
  或 upload_id（分片上传），都不需要公网 URL。

用法:
    python3 tools/qq_media_probe.py --openid <用户openid>
    python3 tools/qq_media_probe.py --openid xxx --mode base64
    python3 tools/qq_media_probe.py --openid xxx --mode all
"""
from __future__ import annotations

import argparse
import base64
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from qq_probe import get_token, load_env  # noqa: E402

API = "https://api.sgroup.qq.com"


def upload(openid: str, payload: dict, token: str, is_group: bool = False) -> tuple[int, dict]:
    path = (f"/v2/groups/{openid}/files" if is_group
            else f"/v2/users/{openid}/files")
    body = json.dumps(payload).encode()
    req = urllib.request.Request(
        API + path,
        data=body,
        headers={
            "Authorization": f"QQBot {token}",
            "Content-Type": "application/json",
            "X-Union-Appid": "1906397285",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode())
        except Exception:
            return e.code, {"raw": "unparseable"}
    except Exception as e:
        return 0, {"error": f"{type(e).__name__}: {e}"}


def main() -> int:
    ap = argparse.ArgumentParser(description="探测 QQ 富媒体上传方式")
    ap.add_argument("--openid", required=True, help="用户/群 openid")
    ap.add_argument("--image", default="data/memory/portraits/placeholder.png")
    ap.add_argument("--mode", default="base64", choices=["base64", "url", "all"])
    ap.add_argument("--group", action="store_true", help="按群接口上传")
    args = ap.parse_args()

    env = load_env(Path(".env.local"))
    token = get_token(env["QQ_APP_ID"], env["QQ_APP_SECRET"]).get("access_token", "")
    if not token:
        print("[x] 取 token 失败", file=sys.stderr)
        return 1

    img = Path(args.image)
    if not img.exists():
        print(f"[x] 图片不存在: {img}", file=sys.stderr)
        return 1
    raw = img.read_bytes()
    print(f"图片: {img} ({len(raw)} 字节)")

    if args.mode in ("base64", "all"):
        print("\n=== 方式 1：二进制直传（file_data）===")
        payload = {
            "file_type": 1,
            "file_name": img.name,
            "file_data": base64.b64encode(raw).decode(),
            "srv_send_msg": False,
        }
        code, resp = upload(args.openid, payload, token, args.group)
        ok = "file_info" in resp
        print(f"  HTTP {code} {'✓ 成功' if ok else '✗ 失败'}")
        print(f"  {json.dumps(resp, ensure_ascii=False)[:300]}")

    if args.mode in ("url", "all"):
        print("\n=== 方式 2：URL 上传 ===")
        payload = {
            "file_type": 1,
            "url": "https://avatars.githubusercontent.com/u/1?v=4",
            "srv_send_msg": False,
        }
        code, resp = upload(args.openid, payload, token, args.group)
        ok = "file_info" in resp
        print(f"  HTTP {code} {'✓ 成功' if ok else '✗ 失败'}")
        print(f"  {json.dumps(resp, ensure_ascii=False)[:200]}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
