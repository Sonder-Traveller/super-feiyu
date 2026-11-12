#!/usr/bin/env python3
"""SSH ProxyCommand：通过 HTTP CONNECT 代理转发 SSH 连接。

背景：本机到 github.com 的 SSH 直连在密钥交换阶段被切断（kex_exchange_identification），
但 HTTP 代理（127.0.0.1:7890）可用。用 CONNECT 把 SSH 流量送进代理隧道。

注意：Windows 的 select() 只接受 socket，不接受 stdin/stdout 管道，
所以这里用双线程搬运，而不是 select。

用法（作为 ProxyCommand）：
    ssh -o "ProxyCommand=python proxy_connect.py %h %p" -p 443 git@ssh.github.com
"""

import socket
import sys
import threading

PROXY_HOST = "127.0.0.1"
PROXY_PORT = 7890
TIMEOUT = 20
CHUNK = 65536


def pump_stdin_to_sock(sock: socket.socket) -> None:
    """把本地 stdin 送往远端，读到 EOF 后关闭写方向。"""
    stdin = sys.stdin.buffer
    try:
        while True:
            data = stdin.read(CHUNK)
            if not data:
                break
            sock.sendall(data)
    except OSError:
        pass
    finally:
        try:
            sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: proxy_connect.py <host> <port>", file=sys.stderr)
        return 2

    target_host, target_port = sys.argv[1], int(sys.argv[2])

    sock = socket.create_connection((PROXY_HOST, PROXY_PORT), timeout=TIMEOUT)
    sock.settimeout(TIMEOUT)

    req = (
        f"CONNECT {target_host}:{target_port} HTTP/1.1\r\n"
        f"Host: {target_host}:{target_port}\r\n"
        "Proxy-Connection: keep-alive\r\n\r\n"
    )
    sock.sendall(req.encode())

    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            print("proxy closed during CONNECT", file=sys.stderr)
            return 1
        buf += chunk

    status_line = buf.split(b"\r\n", 1)[0].decode(errors="replace")
    if "200" not in status_line:
        print(f"CONNECT failed: {status_line}", file=sys.stderr)
        return 1

    sock.settimeout(None)  # 隧道建立后交给阻塞 IO

    writer = threading.Thread(target=pump_stdin_to_sock, args=(sock,), daemon=True)
    writer.start()

    stdout = sys.stdout.buffer
    try:
        while True:
            data = sock.recv(CHUNK)
            if not data:
                break
            stdout.write(data)
            stdout.flush()
    except OSError:
        pass
    finally:
        try:
            sock.close()
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
