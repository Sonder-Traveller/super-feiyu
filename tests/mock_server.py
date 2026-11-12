#!/usr/bin/env python3
"""本地模拟服务器：验证 super-feiyu 的 HTTP 链路与多 provider 抽象。

不需要真实 API key。模拟：
  POST /v1/embeddings     OpenAI 兼容 embedding
  POST /v1/chat/completions  OpenAI 兼容对话
  POST /chat/completions     同上（deepseek base_url 不带 /v1 的情况）

用法: python3 tests/mock_server.py [端口]
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {"chat_calls": 0, "embedding_calls": 0}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("[mock] " + (fmt % args) + "\n")

    def _send(self, code, payload):
        body = json.dumps(payload, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            req = json.loads(raw)
        except Exception:
            req = {}

        # 校验鉴权头存在（真实链路要求）
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer ") or len(auth) < 12:
            self._send(401, {"error": {"message": "missing or bad Authorization"}})
            return

        if self.path.endswith("/embeddings"):
            STATE["embedding_calls"] += 1
            model = req.get("model", "")
            text = req.get("input", "")
            # 确定性伪向量：按字符码求和
            dim = 1024
            vec = [((sum(map(ord, text)) + i) % 100) / 100.0 for i in range(dim)]
            self._send(200, {
                "object": "list",
                "model": model,
                "data": [{"object": "embedding", "index": 0, "embedding": vec}],
                "usage": {"prompt_tokens": len(text), "total_tokens": len(text)},
            })
            return

        if self.path.endswith("/chat/completions"):
            STATE["chat_calls"] += 1
            msgs = req.get("messages", [])
            sys_text = ""
            user_text = ""
            history_n = 0
            for m in msgs:
                role = m.get("role")
                content = m.get("content")
                if isinstance(content, list):
                    content = "".join(
                        b.get("text", "") for b in content if b.get("type") == "text"
                    )
                if role == "system":
                    sys_text = content or ""
                elif role == "user":
                    history_n += 1
                    user_text = content or ""

            # 巩固调用：system 里含"记忆整理器"时返回结构化 JSON
            if "记忆整理器" in sys_text:
                payload = {
                    "episodes": [
                        {"summary": "聊了工作和加班", "importance": 0.7}
                    ],
                    "facts": [
                        {"subject": "用户", "content": "在一家做芯片的公司上班",
                         "confidence": 0.9},
                        {"subject": "用户", "content": "最近经常加班到很晚",
                         "confidence": 0.85},
                    ],
                    "traits": [],
                }
                reply = "```json\n" + json.dumps(payload, ensure_ascii=False) + "\n```"
            else:
                reply = (
                    f"[mock] 收到「{user_text}」；system {len(sys_text)} 字；"
                    f"历史 {max(history_n - 1, 0)} 条"
                )
            self._send(200, {
                "id": "mock-1",
                "object": "chat.completion",
                "model": req.get("model", "mock"),
                "choices": [
                    {"index": 0, "message": {"role": "assistant", "content": reply},
                     "finish_reason": "stop"}
                ],
                "usage": {
                    "prompt_tokens": len(sys_text) + len(user_text),
                    "completion_tokens": len(reply),
                    "total_tokens": len(sys_text) + len(user_text) + len(reply),
                    "prompt_tokens_details": {"cached_tokens": len(sys_text)},
                },
            })
            return

        if self.path.endswith("/audio/transcriptions"):
            self._send(200, {"text": "[mock] 这是一段转写"})
            return

        self._send(404, {"error": {"message": "not found", "path": self.path}})

    def do_GET(self):
        if self.path == "/__stats":
            self._send(200, STATE)
            return
        self._send(404, {"error": "not found"})


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"[mock] listening on http://127.0.0.1:{port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

