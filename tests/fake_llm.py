#!/usr/bin/env python3
"""S3a 的假 LLM server：OpenAI 兼容 /chat/completions（非流式）。

行为由请求路径前缀决定（把 base-url 指到哪个前缀就测哪个分支）：
  /chat/completions             200 + 确定性回复
  /error401/chat/completions    401
  /error500/chat/completions    500
  /garbage/chat/completions     200 + 非 JSON 正文
  /slow/chat/completions        先睡 3 秒再回 200
  /failonce/chat/completions    第一次 500，之后正常（验证失败轮次不回灌历史）

回复文本里带消息条数与最后一条用户消息，用来验证 core 的多轮历史管理：
  "fake(3 msgs) echo=第二句"

启动后在 stdout 第一行打印 PORT=<port>，测试据此连上来。
"""
import json
import time

_FAILONCE_HITS = 0
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):  # 安静，别污染测试输出
        pass

    def _send(self, status, body, ctype="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _json(self, status, obj):
        self._send(status, json.dumps(obj, ensure_ascii=False).encode("utf-8"))

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            body = json.loads(raw)
        except Exception:
            body = {}

        parts = [p for p in self.path.split("/") if p]
        mode = parts[0] if len(parts) > 1 else ""

        if mode == "error401":
            return self._json(401, {"error": {"message": "invalid api key", "type": "authentication_error"}})
        if mode == "error500":
            return self._json(500, {"error": {"message": "internal server error", "type": "server_error"}})
        if mode == "garbage":
            return self._send(200, b"this is definitely not json")
        if mode == "slow":
            time.sleep(3)
        if mode == "failonce":
            global _FAILONCE_HITS
            _FAILONCE_HITS += 1
            if _FAILONCE_HITS == 1:
                return self._json(500, {"error": {"message": "transient failure", "type": "server_error"}})

        msgs = body.get("messages") or []
        last = msgs[-1].get("content", "") if msgs else ""
        text = "fake(%d msgs) echo=%s" % (len(msgs), last)
        self._json(200, {
            "id": "fake-1",
            "object": "chat.completion",
            "model": body.get("model", "fake"),
            "choices": [{"index": 0,
                         "message": {"role": "assistant", "content": text},
                         "finish_reason": "stop"}],
            "usage": {"prompt_tokens": len(msgs), "completion_tokens": len(text),
                      "total_tokens": len(msgs) + len(text)},
        })


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    print("PORT=%d" % srv.server_address[1], flush=True)
    srv.serve_forever()
