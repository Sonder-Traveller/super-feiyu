#!/usr/bin/env python3
"""用一个假控制面验证桥接层的凭据拉取链路。

验证点：
1. --control-url 能从控制面拉到凭据，source = "control"
2. 控制面 404（未登录）时退回 cred-file
3. 控制面不可达时退回 cred-file
4. 控制面换账号后，桥能同步到新的那个
5. 控制面令牌错误时不会静默用别的凭据顶替（要能看到失败日志）

用法：python3 test_bridge_control.py
"""
import json
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import workbuddy_bridge as wb  # noqa: E402

TOKEN = "test-credential-token"
STATE = {"cred": None, "auth_ok": True, "status": 200}


class FakeControl(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path != "/v1/capability/workbuddy/credential":
            self.send_response(404)
            self.end_headers()
            return
        auth = self.headers.get("Authorization", "")
        if not STATE["auth_ok"] or auth != f"Bearer {TOKEN}":
            self.send_response(401)
            body = b'{"error":"unauthorized"}'
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if STATE["cred"] is None:
            self.send_response(404)
            body = b'{"error":"no_credential"}'
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        body = json.dumps(STATE["cred"]).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def start_server():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), FakeControl)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv, srv.server_address[1]


def make_cred(token, nickname="测试账号"):
    return {
        "version": 1,
        "access_token": token,
        "refresh_token": "rt-" + token,
        "expires_at": int(time.time()) + 3600,
        "domain": "www.workbuddy.cn",
        "nickname": nickname,
        "uid": "u-1",
        "saved_at": int(time.time() * 1000),
    }


def check(name, cond, extra=""):
    print(f"{'PASS' if cond else 'FAIL'}  {name} {extra}")
    return bool(cond)


def main():
    ok = True
    srv, port = start_server()
    base = f"http://127.0.0.1:{port}"

    with tempfile.TemporaryDirectory() as td:
        cred_path = Path(td) / "workbuddy.json"
        cred_path.write_text(json.dumps(make_cred("local-token", "本地账号")),
                             encoding="utf-8")

        # ── 1. 控制面有凭据 → source = control，优先于 cred-file ──
        STATE["cred"] = make_cred("ctrl-token", "控制面账号")
        src = wb.ControlSource(base, TOKEN)
        acc = src.load()
        ok &= check("控制面拉到凭据", acc is not None)
        ok &= check("source == control", acc and acc.source == "control")
        ok &= check("nickname 正确", acc and acc.nickname == "控制面账号")
        ok &= check("domain 正确", acc and acc.domain == "www.workbuddy.cn")
        ok &= check("expires_at 是秒", acc and acc.expires_at > 10**9)

        # ── 2. 池子里 control 优先 ──
        pool = wb.AccountPool(None, "auto", wb.CredFile(cred_path), src)
        picked = pool.pick()
        ok &= check("pick 优先 control", picked and picked.source == "control")
        ok &= check("whoami.source == control",
                    pool.whoami().get("source") == "control")

        # ── 3. 控制面 404（未登录）→ 退回 cred-file ──
        STATE["cred"] = None
        pool2 = wb.AccountPool(None, "auto", wb.CredFile(cred_path), src)
        picked2 = pool2.pick()
        ok &= check("控制面 404 时退回 cred-file",
                    picked2 and picked2.source == "cred-file",
                    f"(got {picked2.source if picked2 else None})")

        # ── 4. 控制面不可达 → 退回 cred-file ──
        dead = wb.ControlSource("http://127.0.0.1:1", TOKEN)
        pool3 = wb.AccountPool(None, "auto", wb.CredFile(cred_path), dead)
        picked3 = pool3.pick()
        ok &= check("控制面不可达时退回 cred-file",
                    picked3 and picked3.source == "cred-file",
                    f"(got {picked3.source if picked3 else None})")

        # ── 5. 控制面换账号 → 同步到新的 ──
        STATE["cred"] = make_cred("ctrl-token-2", "新账号")
        pool4 = wb.AccountPool(None, "auto", wb.CredFile(cred_path), src)
        pool4.maybe_pull_control_credential(force=True)
        picked4 = pool4.pick()
        ok &= check("控制面换账号后同步",
                    picked4 and picked4.nickname == "新账号",
                    f"(got {picked4.nickname if picked4 else None})")

        # ── 6. 令牌错 → 拉不到，不静默顶替 ──
        STATE["auth_ok"] = False
        bad = wb.ControlSource(base, "wrong-token")
        acc_bad = bad.load()
        ok &= check("令牌错误时拉不到凭据", acc_bad is None)
        pool5 = wb.AccountPool(None, "auto", wb.CredFile(cred_path), bad)
        picked5 = pool5.pick()
        ok &= check("令牌错误时退回 cred-file（不冒充 control）",
                    picked5 and picked5.source == "cred-file",
                    f"(got {picked5.source if picked5 else None})")
        STATE["auth_ok"] = True

    srv.shutdown()
    print("\n=== ALL PASS ===" if ok else "\n=== SOME FAILED ===")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
