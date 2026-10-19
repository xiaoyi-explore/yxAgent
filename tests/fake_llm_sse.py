#!/usr/bin/env python3
"""S3b 的假 LLM server：OpenAI 兼容 /chat/completions，**流式**（text/event-stream）。

为什么单独一个文件而不是改 fake_llm.py：S3a 的假 server 是那个阶段的验收基线，
S3b 不许动它（"S1/S2/S3a 回归必须保持全绿"的第一条纪律就是不碰老的 fixture 与假 server）。

行为由请求路径前缀决定（把 base-url 指到哪个前缀就测哪个分支）：
  /chat/completions             按事件自然切块（基线）
  /splitjson/...                字节流切在一个 JSON 事件的正中间
  /splitutf8/...                字节流切在一个 UTF-8 码点的正中间（中文流式必踩）
  /comment/...                  带 `: keep-alive` 注释行与多行 data
  /nousage/...                  流里没有 usage（OpenAI 默认行为）
  /nodone/...                   没有 [DONE]、最后一个事件也没有空行收尾
  /slow/...                     事件之间 sleep 50ms
  /abort/...                    发两个事件后 RST 掉连接（中途断连）
  /droponce/...                 第一次请求直接掐断（不回任何 HTTP），之后正常（验证传输层重试）
  /error401/... /error429/... /error500/...   对应状态码 + JSON 错误体

另外记下每个请求的形参，测试用 `GET /__last` 回读（验证 stream:true 与 include_usage）。

启动后在 stdout 第一行打印 PORT=<port>；fixture 从 tests/fixtures/*.sse 读。
"""
import json
import os
import socket
import struct
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

FIXTURES = Path(__file__).resolve().parent / "fixtures"

MODE_TO_FIXTURE = {
    "": "sse_basic.sse",
    "stream": "sse_basic.sse",
    "splitjson": "sse_basic.sse",
    "splitutf8": "sse_basic.sse",
    "slow": "sse_basic.sse",
    "abort": "sse_basic.sse",
    "droponce": "sse_basic.sse",
    "comment": "sse_comment.sse",
    "nousage": "sse_nousage.sse",
    "nodone": "sse_nodone.sse",
}
ERROR_MODES = {"error401": 401, "error429": 429, "error500": 500}

DROPONCE_HITS = 0
LAST = {}


def event_pieces(data):
    """按事件边界（空行）切块，但**一个字节都不改动**——fixture 长什么样就发什么样。"""
    out, start = [], 0
    while True:
        i = data.find(b"\n\n", start)
        if i < 0:
            if start < len(data):
                out.append(data[start:])
            return out
        out.append(data[start:i + 2])
        start = i + 2


def split_at(data, mode):
    """把整段字节切成两块，切点故意落在最难的位置上。"""
    if mode == "splitjson":
        first = data.find(b"data: ")
        second = data.find(b"data: ", first + 1)
        cut = data.find(b'"object"', second)
        if cut > 0:
            return [data[:cut + 5], data[cut + 5:]]
    elif mode == "splitutf8":
        for i, b in enumerate(data):
            if b >= 0x80:  # 第一个非 ASCII 字节 = 某个多字节码点的开头
                return [data[:i + 1], data[i + 1:]]
    return [data]


def render(mode, body):
    msgs = body.get("messages") or []
    echo = msgs[-1].get("content", "") if msgs else ""
    text = "fake(%d msgs) echo=%s" % (len(msgs), echo)
    esc = json.dumps(echo, ensure_ascii=False)[1:-1]  # 去掉外层引号：填进 JSON 字符串里
    raw = (FIXTURES / MODE_TO_FIXTURE[mode]).read_text(encoding="utf-8")
    raw = (raw.replace("{{MSGS}}", str(len(msgs))).replace("{{ECHO}}", esc)
              .replace("{{CLEN}}", str(len(text))).replace("{{TLEN}}", str(len(msgs) + len(text))))
    return raw.encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):  # 安静，别污染测试输出
        pass

    # ---- 非流式端点：给测试回读"上一次请求到底发了什么" ----
    def do_GET(self):
        if urllib.parse.urlparse(self.path).path == "/__last":
            body = json.dumps(LAST, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)

    def _json(self, status, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _rst(self):
        """真的断掉：SO_LINGER=0 让 close 变成 RST，客户端读到一个硬错误而不是干净的 EOF。"""
        try:
            self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            self.connection.close()
        except Exception:
            pass
        self.close_connection = True

    def _stream(self, pieces, delay=0.0, abort_after=None):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        try:
            for n, piece in enumerate(pieces):
                if abort_after is not None and n >= abort_after:
                    self._rst()
                    return
                self.wfile.write(b"%x\r\n" % len(piece) + piece + b"\r\n")
                self.wfile.flush()  # 必须 flush：否则"流式"退化成攒完一次吐
                if delay:
                    time.sleep(delay)
            self.wfile.write(b"0\r\n\r\n")  # 终止块
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    def do_POST(self):
        global DROPONCE_HITS
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            body = json.loads(raw)
        except Exception:
            body = {}

        parts = [p for p in self.path.split("/") if p]
        mode = parts[0] if len(parts) > 1 else ""
        LAST.clear()
        LAST.update({
            "path": self.path,
            "mode": mode,
            "stream": body.get("stream"),
            "stream_options": body.get("stream_options"),
            "model": body.get("model"),
            "messages": len(body.get("messages") or []),
            "authorization": self.headers.get("Authorization"),
        })

        if mode in ERROR_MODES:
            return self._json(ERROR_MODES[mode], {"error": {"message": "fake %s" % mode, "type": "server_error"}})

        if mode == "droponce":
            DROPONCE_HITS += 1
            if DROPONCE_HITS == 1:
                return self._rst()  # 连 HTTP 响应都不给：这才是"传输层失败"

        if mode not in MODE_TO_FIXTURE:
            return self._json(404, {"error": {"message": "no such mode: %s" % mode}})

        data = render(mode, body)
        if mode in ("splitjson", "splitutf8"):
            return self._stream(split_at(data, mode))
        if mode == "slow":
            return self._stream(event_pieces(data), delay=0.05)
        if mode == "abort":
            return self._stream(event_pieces(data), abort_after=2)
        return self._stream(event_pieces(data))


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    print("PORT=%d" % srv.server_address[1], flush=True)
    srv.serve_forever()
