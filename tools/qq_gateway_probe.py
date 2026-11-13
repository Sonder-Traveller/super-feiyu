#!/usr/bin/env python3
"""测试 QQ 机器人 WebSocket 网关能否连上并完成 IDENTIFY。

「连接中一直转圈」通常卡在这里：token 拿到了，但 gateway 连不上
或 IDENTIFY 被拒。

用法:
    python3 tools/qq_gateway_probe.py
"""
from __future__ import annotations

import json
import ssl
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from qq_probe import api_get, get_token, load_env  # noqa: E402


def main() -> int:
    env = load_env(Path(".env.local"))
    app_id = env.get("QQ_APP_ID", "")
    secret = env.get("QQ_APP_SECRET", "")

    print("[1] 获取 access_token …")
    d = get_token(app_id, secret)
    token = d.get("access_token")
    if not token:
        print(f"    [x] 失败: {d}")
        return 1
    print(f"    [✓] expires_in={d.get('expires_in')}s")

    print("[2] 查询 gateway 地址 …")
    code, body = api_get("/gateway", token)
    print(f"    HTTP {code}: {body[:200]}")
    if code != 200:
        print("    [x] 拿不到 gateway URL")
        return 1
    ws_url = json.loads(body).get("url", "")
    print(f"    [✓] {ws_url}")

    print("[3] 建立 WebSocket 连接 …")
    try:
        from websocket import create_connection  # websocket-client
    except ImportError:
        print("    [!] 未安装 websocket-client，改用 curl 测试")
        return _test_with_curl(ws_url)

    try:
        ctx = ssl.create_default_context()
        ws = create_connection(ws_url, timeout=15, sslopt={"context": ctx})
        print("    [✓] TCP/TLS 已连上")
    except Exception as e:
        print(f"    [x] 连接失败: {type(e).__name__}: {e}")
        return 1

    print("[4] 等 HELLO (op=10) …")
    try:
        raw = ws.recv()
        hello = json.loads(raw)
        print(f"    [✓] op={hello.get('op')} "
              f"heartbeat_interval={hello.get('d', {}).get('heartbeat_interval')}ms")
    except Exception as e:
        print(f"    [x] 没收到 HELLO: {e}")
        ws.close()
        return 1

    print("[5] 发 IDENTIFY (op=2) …")
    identify = {
        "op": 2,
        "d": {
            "token": f"QQBot {token}",
            "intents": (1 << 25) | (1 << 30),  # GROUP_AND_C2C + PUBLIC_MESSAGES
            "properties": {"$os": "linux", "$browser": "super-feiyu",
                           "$device": "super-feiyu"},
        },
    }
    ws.send(json.dumps(identify))
    print("    已发送，等 READY …")

    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            raw = ws.recv()
        except Exception as e:
            print(f"    [x] 接收失败: {e}")
            ws.close()
            return 1
        try:
            ev = json.loads(raw)
        except Exception:
            continue
        op = ev.get("op")
        t = ev.get("t")
        if op == 0 and t == "READY":
            d = ev.get("d", {})
            user = d.get("user", {})
            print(f"    [✓] READY! bot={user.get('username')} "
                  f"id={user.get('id')}")
            print(f"        session_id={d.get('session_id', '')[:20]}…")
            print(f"        shard={d.get('shard')}")
            ws.close()
            return 0
        if op == 9:
            print(f"    [x] INVALID_SESSION: {ev.get('d')}")
            ws.close()
            return 1
        if op == 7:
            print("    [!] RECONNECT 要求重连")
        print(f"    … op={op} t={t}")

    print("    [x] 15s 内没等到 READY")
    ws.close()
    return 1


def _test_with_curl(ws_url: str) -> int:
    import subprocess

    print(f"    curl 探测 {ws_url} …")
    r = subprocess.run(
        ["curl", "-sS", "-i", "-N", "--max-time", "8",
         "-H", "Connection: Upgrade",
         "-H", "Upgrade: websocket",
         "-H", "Sec-WebSocket-Version: 13",
         "-H", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==",
         ws_url],
        capture_output=True, text=True, timeout=20,
    )
    print(f"    exit={r.returncode}")
    print(f"    stdout: {r.stdout[:600]}")
    print(f"    stderr: {r.stderr[:300]}")
    return 0 if "101" in r.stdout else 1


if __name__ == "__main__":
    sys.exit(main())
