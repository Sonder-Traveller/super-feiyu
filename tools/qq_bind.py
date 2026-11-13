#!/usr/bin/env python3
"""QQ 官方机器人扫码绑定工具。

借鉴腾讯官方 SDK `@tencent-connect/qqbot-connector` 的协议实现，
但不依赖 Node.js —— 直接用 Python 完成同一套 HTTP 流程。

协议（实测自 SDK 源码）：
  1. POST https://q.qq.com/lite/create_bind_task
       请求 {"key": <32 随机字节的 base64>}
       响应 {"retcode":0,"data":{"task_id":"..."}}
  2. 二维码内容：
       https://q.qq.com/qqbot/openclaw/connect.html?task_id=<id>&source=<src>&_wv=2
  3. 轮询 POST https://q.qq.com/lite/poll_bind_result
       请求 {"task_id": <id>}
       响应 data.status: 0=NONE 1=PENDING 2=COMPLETED 3=EXPIRED
            完成时含 bot_appid / bot_encrypt_secret / user_openid
  4. AppSecret 解密：
       AES-256-GCM，key = base64decode(key)（32 字节）
       密文布局 = nonce(12) + ciphertext + tag(16)

用法:
    python3 tools/qq_bind.py                 # 交互式：出二维码，等扫码
    python3 tools/qq_bind.py --qr-out qr.png # 同时存二维码图片
    python3 tools/qq_bind.py --source dsh    # 自定义来源标识
    python3 tools/qq_bind.py --write-config  # 成功后直接写入 config.toml 的 .env

安全：
    - 返回的 AppSecret 只打印一次，不写日志
    - 建议随后立即写入 .env 并 chmod 600
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HOST = "q.qq.com"
CREATE_URL = f"https://{HOST}/lite/create_bind_task"
POLL_URL = f"https://{HOST}/lite/poll_bind_result"
CONNECT_URL = f"https://{HOST}/qqbot/openclaw/connect.html"

# BindStatus（与 SDK 一致）
NONE, PENDING, COMPLETED, EXPIRED = 0, 1, 2, 3

STATUS_TEXT = {
    NONE: "无任务",
    PENDING: "等待扫码",
    COMPLETED: "已绑定",
    EXPIRED: "二维码过期",
}


def _post(url: str, payload: dict, timeout: float = 10.0,
          retries: int = 4) -> dict:
    """POST JSON，带指数退避重试。

    WSL 里 q.qq.com 偶发 TLS UNEXPECTED_EOF（连接被中途掐断），
    重试即可恢复；4xx 属于请求本身有问题，不重试。
    """
    body = json.dumps(payload).encode()
    last: Exception | None = None
    for attempt in range(retries):
        req = urllib.request.Request(
            url,
            data=body,
            headers={
                "Content-Type": "application/json",
                "Accept": "application/json",
                "Content-Length": str(len(body)),
                "User-Agent": "super-feiyu-qq-bind/1.0",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                if r.status != 200:
                    raise RuntimeError(f"HTTP {r.status} from {url}")
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            # 4xx 是客户端问题，重试没意义
            if 400 <= e.code < 500:
                raise
            last = e
        except (urllib.error.URLError, TimeoutError, ConnectionError,
                json.JSONDecodeError) as e:
            last = e
        if attempt < retries - 1:
            time.sleep(0.4 * (2 ** attempt))
    raise RuntimeError(f"请求 {url} 失败（重试 {retries} 次）: {last}") from last


def create_bind_task() -> tuple[str, str]:
    """创建绑定任务，返回 (task_id, key_b64)。"""
    key_b64 = base64.b64encode(secrets.token_bytes(32)).decode()
    resp = _post(CREATE_URL, {"key": key_b64})
    if resp.get("retcode") != 0:
        raise RuntimeError(f"create_bind_task failed: {resp.get('msg')}")
    task_id = (resp.get("data") or {}).get("task_id")
    if not task_id:
        raise RuntimeError("create_bind_task: missing task_id")
    return task_id, key_b64


def build_connect_url(task_id: str, source: str = "") -> str:
    from urllib.parse import quote
    return (f"{CONNECT_URL}?task_id={quote(task_id)}"
            f"&source={quote(source)}&_wv=2")


def poll_bind_result(task_id: str) -> dict:
    resp = _post(POLL_URL, {"task_id": task_id})
    if resp.get("retcode") != 0:
        raise RuntimeError(f"poll_bind_result failed: {resp.get('msg')}")
    d = resp.get("data") or {}
    return {
        "status": d.get("status", NONE),
        "app_id": str(d.get("bot_appid") or ""),
        "encrypt_secret": d.get("bot_encrypt_secret") or "",
        "user_openid": d.get("user_openid") or "",
    }


def decrypt_secret(encrypt_b64: str, key_b64: str) -> str:
    """AES-256-GCM 解密。密文 = nonce(12) + ciphertext + tag(16)。"""
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        # 没有 cryptography 时，用 openssl 命令行兜底
        return _decrypt_with_openssl(encrypt_b64, key_b64)

    key = base64.b64decode(key_b64)
    blob = base64.b64decode(encrypt_b64)
    nonce, ct, tag = blob[:12], blob[12:-16], blob[-16:]
    aes = AESGCM(key)
    # AESGCM 需要 ciphertext||tag 的形式
    return aes.decrypt(nonce, ct + tag, None).decode("utf-8")


def _decrypt_with_openssl(encrypt_b64: str, key_b64: str) -> str:
    """无 cryptography 库时的兜底：调用 openssl enc -d -aes-256-gcm。"""
    import subprocess
    import tempfile

    key = base64.b64decode(key_b64)
    blob = base64.b64decode(encrypt_b64)
    nonce, ct, tag = blob[:12], blob[12:-16], blob[-16:]

    # openssl 的 GCM 不支持直接指定 tag，改用 python 内置实现兜底
    # 这里退化为纯 Python AES-GCM（仅当 cryptography 缺失时使用）
    return _pure_python_aes_gcm(key, nonce, ct, tag)


def _pure_python_aes_gcm(key: bytes, nonce: bytes, ct: bytes, tag: bytes) -> str:
    """最小 AES-256-GCM 实现（仅用于 cryptography 缺失的兜底）。

    依赖 pycryptodome（多数环境已有）；没有则明确报错，不静默失败。
    """
    try:
        from Crypto.Cipher import AES
    except ImportError:
        raise RuntimeError(
            "解密 AppSecret 需要 cryptography 或 pycryptodome。\n"
            "安装其一：\n"
            "  pip install cryptography\n"
            "  pip install pycryptodome"
        ) from None

    cipher = AES.new(key, AES.MODE_GCM, nonce=nonce)
    plain = cipher.decrypt_and_verify(ct, tag)
    return plain.decode("utf-8")


def render_qr_terminal(text: str) -> str:
    """在终端渲染二维码（纯 Python，无第三方依赖）。"""
    try:
        import qrcode
        qr = qrcode.QRCode(border=1)
        qr.add_data(text)
        qr.make(fit=True)
        buf = []
        m = qr.get_matrix()
        for row in m:
            buf.append("".join("██" if cell else "  " for cell in row))
        return "\n".join(buf)
    except ImportError:
        return ""


def save_qr_png(text: str, path: Path) -> bool:
    try:
        import qrcode
        qr = qrcode.QRCode(border=2, box_size=8)
        qr.add_data(text)
        qr.make(fit=True)
        img = qr.make_image(fill_color="black", back_color="white")
        img.save(str(path))
        return True
    except Exception:
        return False


def main() -> int:
    ap = argparse.ArgumentParser(description="QQ 官方机器人扫码绑定")
    ap.add_argument("--source", default="super-feiyu", help="来源标识（显示在扫码页）")
    ap.add_argument("--qr-out", default=None, help="把二维码存成 PNG")
    ap.add_argument("--timeout", type=int, default=300, help="最长等待秒数")
    ap.add_argument("--interval", type=float, default=2.0, help="轮询间隔秒")
    ap.add_argument("--write-env", default=None,
                    help="成功后把凭据写入指定 .env 文件")
    args = ap.parse_args()

    print("=" * 60)
    print("QQ 官方机器人扫码绑定")
    print("=" * 60)

    try:
        task_id, key_b64 = create_bind_task()
    except Exception as e:
        print(f"[x] 创建绑定任务失败: {e}", file=sys.stderr)
        print("    检查网络：q.qq.com 是否可达", file=sys.stderr)
        return 1

    url = build_connect_url(task_id, args.source)
    print(f"\n二维码内容：\n{url}\n")

    if args.qr_out:
        if save_qr_png(url, Path(args.qr_out)):
            print(f"[✓] 二维码已保存: {args.qr_out}")
        else:
            print("[!] 保存二维码失败（需要 pip install qrcode[pil]）")

    art = render_qr_terminal(url)
    if art:
        print(art)
        print()
    print("请用手机 QQ 扫描上面的二维码完成绑定。")
    print("（二维码过期会自动刷新）\n")

    deadline = time.time() + args.timeout
    last_status = None
    try:
        while time.time() < deadline:
            try:
                r = poll_bind_result(task_id)
            except Exception as e:
                print(f"[!] 轮询出错，重试: {e}", file=sys.stderr)
                time.sleep(args.interval)
                continue

            st = r["status"]
            if st != last_status:
                print(f"    状态: {STATUS_TEXT.get(st, st)}")
                last_status = st

            if st == COMPLETED:
                app_id = r["app_id"]
                try:
                    secret = decrypt_secret(r["encrypt_secret"], key_b64)
                except Exception as e:
                    print(f"[x] AppSecret 解密失败: {e}", file=sys.stderr)
                    print(f"    appId 已拿到: {app_id}", file=sys.stderr)
                    return 1

                print("\n" + "=" * 60)
                print("绑定成功")
                print("=" * 60)
                print(f"  AppID     : {app_id}")
                print(f"  AppSecret : {secret}")
                if r.get("user_openid"):
                    print(f"  绑定账号   : {r['user_openid'][:12]}…")
                print()
                print("把这两项写进 .env：")
                print(f"  QQ_APP_ID={app_id}")
                print(f"  QQ_APP_SECRET={secret}")

                if args.write_env:
                    p = Path(args.write_env)
                    lines = []
                    if p.exists():
                        lines = [ln for ln in p.read_text(encoding="utf-8").splitlines()
                                 if not ln.startswith(("QQ_APP_ID=", "QQ_APP_SECRET="))]
                    lines.append(f"QQ_APP_ID={app_id}")
                    lines.append(f"QQ_APP_SECRET={secret}")
                    p.write_text("\n".join(lines) + "\n", encoding="utf-8")
                    try:
                        os.chmod(p, 0o600)
                    except OSError:
                        pass
                    print(f"\n[✓] 已写入 {p}（权限 600）")

                print("\n下一步：")
                print("  1. 在 config.toml 里启用 channel.qq_official")
                print("  2. 去 q.qq.com 配沙箱（群名要含「测试」才能拉机器人进群）")
                print("  3. 启动 super-feiyu")
                return 0

            if st == EXPIRED:
                print("    二维码过期，刷新中…")
                try:
                    task_id, key_b64 = create_bind_task()
                    url = build_connect_url(task_id, args.source)
                    print(f"\n新的二维码：\n{url}\n")
                    if args.qr_out:
                        save_qr_png(url, Path(args.qr_out))
                    art = render_qr_terminal(url)
                    if art:
                        print(art)
                except Exception as e:
                    print(f"[x] 刷新失败: {e}", file=sys.stderr)
                    return 1

            time.sleep(args.interval)

        print("\n[x] 超时未扫码", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\n[!] 已取消")
        return 130


if __name__ == "__main__":
    sys.exit(main())
