#!/usr/bin/env python3
"""端到端：真的起一个桥接层进程，让它从假控制面拉凭据。

验证的是「进程级」的行为，不是类级：
1. 桥带 --control-url 启动，从控制面拿到凭据
2. /v1/whoami 报 source = "control"（而不是 none / vault）
3. /v1/health 报账号数
4. 控制面换成另一个账号后，桥同步过去

用法：python3 test_bridge_e2e.py
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOKEN = "e2e-token"
STATE = {"cred": None}


class FakeControl(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path != "/v1/capability/workbuddy/credential":
            self.send_response(404)
            self.end_headers()
            return
        if self.headers.get("Authorization") != f"Bearer {TOKEN}":
            self.send_response(401)
            body = b'{"error":"unauthorized"}'
        elif STATE["cred"] is None:
            self.send_response(404)
            body = b'{"error":"no_credential"}'
        else:
            body = json.dumps(STATE["cred"]).encode("utf-8")
            self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def get(url, timeout=5):
    import urllib.request
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def check(name, cond, extra=""):
    print(f"{'PASS' if cond else 'FAIL'}  {name} {extra}")
    return bool(cond)


def make_cred(token, nickname):
    return {"version": 1, "access_token": token, "refresh_token": "rt-" + token,
            "expires_at": int(time.time()) + 3600, "domain": "www.workbuddy.cn",
            "nickname": nickname, "uid": "u-1",
            "saved_at": int(time.time() * 1000)}


def main():
    ok = True
    ctrl_srv = ThreadingHTTPServer(("127.0.0.1", 0), FakeControl)
    threading.Thread(target=ctrl_srv.serve_forever, daemon=True).start()
    ctrl_port = ctrl_srv.server_address[1]

    bridge_port = free_port()
    STATE["cred"] = make_cred("tok-1", "账号一")

    with tempfile.TemporaryDirectory() as td:
        logf = open(Path(td) / "bridge.log", "w", encoding="utf-8")
        env = dict(os.environ)
        env["FEIYU_CREDENTIAL_TOKEN"] = TOKEN
        # 故意不给 --vault / --cred-file：只靠控制面这一个来源
        proc = subprocess.Popen(
            [sys.executable, str(HERE / "workbuddy_bridge.py"),
             "--port", str(bridge_port),
             "--control-url", f"http://127.0.0.1:{ctrl_port}",
             "--vault", str(Path(td) / "no-such-vault")],
            stdout=logf, stderr=subprocess.STDOUT, env=env, text=True)
        try:
            # 等桥起来
            base = f"http://127.0.0.1:{bridge_port}"
            for _ in range(50):
                try:
                    get(base + "/v1/health")
                    break
                except Exception:
                    time.sleep(0.1)
            else:
                print("FAIL  桥没起来")
                return 1

            who = get(base + "/v1/whoami")
            ok &= check("whoami.source == control", who.get("source") == "control",
                        f"(got {who.get('source')})")
            ok &= check("whoami.nickname == 账号一", who.get("nickname") == "账号一",
                        f"(got {who.get('nickname')})")
            ok &= check("whoami.logged_in", who.get("logged_in") is True)

            health = get(base + "/v1/health")
            ok &= check("health 报 1 个账号", health.get("accounts") == 1,
                        f"(got {health.get('accounts')})")
            ok &= check("health 明细 source == control",
                        health["detail"][0]["source"] == "control")

            # 换账号 → 桥应同步（需要等过 5 秒节流窗口）
            STATE["cred"] = make_cred("tok-2", "账号二")
            deadline = time.time() + 15
            who2 = {}
            while time.time() < deadline:
                time.sleep(0.5)
                who2 = get(base + "/v1/whoami")
                if who2.get("nickname") == "账号二":
                    break
            ok &= check("控制面换账号后桥同步",
                        who2.get("nickname") == "账号二",
                        f"(got {who2.get('nickname')})")
            ok &= check("同步后 source 仍是 control", who2.get("source") == "control")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()
            logf.close()
            ctrl_srv.shutdown()
            print("\n--- 桥接层启动日志 ---")
            print((Path(td) / "bridge.log").read_text(encoding="utf-8"))

    print("\n=== ALL PASS ===" if ok else "\n=== SOME FAILED ===")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
