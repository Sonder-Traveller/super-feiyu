#!/usr/bin/env python3
"""模拟 OneBot v11 WebSocket 服务端，用于端到端验证收消息链路。

不依赖 NapCat / 真实 QQ。启动后主动推送几条群消息事件，
super-feiyu 连上来应该能收到并处理。

用法: python3 tests/mock_onebot_ws.py [端口]
"""
import base64
import hashlib
import json
import socket
import struct
import sys
import threading
import time

WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_frame(sock):
    """读一个客户端帧，返回 (opcode, payload) 或 None。"""
    hdr = recv_exact(sock, 2)
    if not hdr:
        return None
    b1, b2 = hdr[0], hdr[1]
    opcode = b1 & 0x0F
    masked = b2 & 0x80
    length = b2 & 0x7F
    if length == 126:
        ext = recv_exact(sock, 2)
        if not ext:
            return None
        length = struct.unpack(">H", ext)[0]
    elif length == 127:
        ext = recv_exact(sock, 8)
        if not ext:
            return None
        length = struct.unpack(">Q", ext)[0]
    mask = recv_exact(sock, 4) if masked else b"\x00\x00\x00\x00"
    if masked and mask is None:
        return None
    payload = recv_exact(sock, length) if length else b""
    if payload is None:
        return None
    if masked:
        payload = bytes(payload[i] ^ mask[i % 4] for i in range(len(payload)))
    return opcode, payload


def make_frame(text, opcode=0x1):
    data = text.encode("utf-8")
    header = bytearray()
    header.append(0x80 | opcode)
    n = len(data)
    if n < 126:
        header.append(n)
    elif n < 65536:
        header.append(126)
        header += struct.pack(">H", n)
    else:
        header.append(127)
        header += struct.pack(">Q", n)
    return bytes(header) + data


def handle_client(conn, addr, events):
    try:
        # 握手
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = conn.recv(4096)
            if not chunk:
                return
            req += chunk
        headers = {}
        for line in req.decode(errors="replace").split("\r\n")[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        key = headers.get("sec-websocket-key", "")
        accept = base64.b64encode(
            hashlib.sha1((key + WS_MAGIC).encode()).digest()
        ).decode()
        resp = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
        )
        conn.sendall(resp.encode())
        print(f"[mock-onebot] client connected from {addr}", flush=True)

        # 推送事件
        for ev in events:
            time.sleep(0.3)
            conn.sendall(make_frame(json.dumps(ev, ensure_ascii=False)))
            print(f"[mock-onebot] pushed {ev.get('message_type')} "
                  f"msg_id={ev.get('message_id')}", flush=True)

        # 保持连接，处理客户端关闭帧
        while True:
            f = read_frame(conn)
            if f is None:
                break
            opcode, payload = f
            if opcode == 0x8:  # close
                break
    except Exception as e:
        print(f"[mock-onebot] client error: {e}", flush=True)
    finally:
        try:
            conn.close()
        except Exception:
            pass
        print("[mock-onebot] client disconnected", flush=True)


def http_server(port, sent_log):
    """同端口提供一个极简 OneBot HTTP API，用于验证发送链路。"""
    import http.server

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(n) if n else b"{}"
            try:
                payload = json.loads(body)
            except Exception:
                payload = {}
            sent_log.append((self.path, payload))
            print(f"[mock-onebot] HTTP {self.path} -> {payload}", flush=True)
            out = json.dumps({"status": "ok", "retcode": 0,
                              "data": {"message_id": 12345}}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)

    http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18082

    events = [
        {  # 群聊，未 @ → 应静默
            "post_type": "message", "message_type": "group",
            "group_id": 1001, "user_id": 2001, "message_id": 3001,
            "time": int(time.time()),
            "sender": {"nickname": "路人甲"},
            "message": [{"type": "text", "data": {"text": "今天天气不错"}}],
        },
        {  # 群聊，@机器人 → 应回复
            "post_type": "message", "message_type": "group",
            "group_id": 1001, "user_id": 2002, "message_id": 3002,
            "time": int(time.time()),
            "sender": {"nickname": "老王"},
            "message": [
                {"type": "at", "data": {"qq": "10086"}},
                {"type": "text", "data": {"text": " 在吗"}},
            ],
        },
        {  # 私聊 → 应回复
            "post_type": "message", "message_type": "private",
            "user_id": 2003, "message_id": 3003,
            "time": int(time.time()),
            "sender": {"nickname": "小李"},
            "message": [{"type": "text", "data": {"text": "你好"}}],
        },
    ]

    sent_log = []
    threading.Thread(target=http_server, args=(port, sent_log), daemon=True).start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)
    print(f"[mock-onebot] listening on ws://127.0.0.1:{port}", flush=True)

    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle_client, args=(conn, addr, events), daemon=True).start()


if __name__ == "__main__":
    main()

