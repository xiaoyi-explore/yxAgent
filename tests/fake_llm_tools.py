#!/usr/bin/env python3
"""S4 的假 LLM server：OpenAI 兼容 /chat/completions，**带工具调用**。

它是有状态的——但不是存在进程里，而是"看请求"：
  * 最后一条消息不是 tool  -> 模型要调工具（返回 tool_calls）
  * 最后一条消息是 tool    -> 模型已经拿到结果，收尾作答（把工具的输出编进答案里）
这样测的才是真正的循环：core 必须把 assistant(tool_calls) 和 tool 消息原样送回来。

行为由请求路径前缀决定：
  /tools/...       一次 read_file，arguments 一整块到达
  /toolsplit/...   arguments 分三片、工具名分两片，其中一片还切在 JSON 正中间（S4 的头号坑）
  /multitool/...   一轮要两个工具（index 0/1 交错到达）
  /toolerr/...     读一个不存在的文件 -> 工具失败必须回灌给模型
  /badargs/...     arguments 是坏 JSON -> 不许执行工具，但要把错误告诉模型
  /shell/...       调 run_shell
  /sleeptool/...   调 run_shell 跑一个会超时的命令（验证看门狗）
  /write/...       调 write_file
  /loop/...        永远要工具，不收尾（验证 MAX_STEPS）

每个请求都会记到 LAST，测试用 GET /__last 回读（验证 tools 声明有没有真的发出去）。

启动后在 stdout 第一行打印 PORT=<port>。
"""
import json
import socket
import struct
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NL = chr(10)
B_NL2 = bytes([10, 10])
LAST = {}


def sse(obj):
    return b"data: " + json.dumps(obj, ensure_ascii=False).encode("utf-8") + B_NL2


def chunk(delta, finish=None):
    c = {"index": 0, "delta": delta}
    if finish is not None:
        c["finish_reason"] = finish
    return {"id": "s4", "object": "chat.completion.chunk", "model": "fake", "choices": [c]}


def usage_chunk(prompt_tokens, completion_tokens):
    return {"id": "s4", "object": "chat.completion.chunk", "model": "fake", "choices": [],
            "usage": {"prompt_tokens": prompt_tokens,
                      "completion_tokens": completion_tokens,
                      "total_tokens": prompt_tokens + completion_tokens}}


# ---- 工具调用的"剧本" ----
def tool_plan(mode):
    """返回 [(call_id, name, arguments_json_text), ...]；None 表示这一轮不收尾（loop 模式）。"""
    if mode == "multitool":
        return [("call_1", "read_file", json.dumps({"path": "a.txt"})),
                ("call_2", "run_shell", json.dumps({"command": "echo second"}))]
    if mode == "toolerr":
        return [("call_1", "read_file", json.dumps({"path": "no-such-file.txt"}))]
    if mode == "badargs":
        return [("call_1", "read_file", '{"path": "a.txt"')]  # 少一个右括号：坏 JSON
    if mode == "shell":
        return [("call_1", "run_shell", json.dumps({"command": "printf 'hi from shell'"}))]
    if mode == "sleeptool":
        return [("call_1", "run_shell", json.dumps({"command": "sleep 5", "timeout_ms": 300}))]
    if mode == "write":
        return [("call_1", "write_file", json.dumps({"path": "out.txt", "content": "written by tool" + NL}))]
    if mode == "loop":
        return [("call_1", "read_file", json.dumps({"path": "a.txt"}))]
    return [("call_1", "read_file", json.dumps({"path": "a.txt"}))]


def count_lines(text):
    if not text:
        return 0
    n = text.count(NL)
    return n if text.endswith(NL) else n + 1


def final_answer(mode, messages):
    tools = [m for m in messages if m.get("role") == "tool"]
    content = tools[-1].get("content", "") if tools else ""
    if mode == "multitool":
        joined = " / ".join((m.get("content", "") or "").strip().split(NL)[0] for m in tools)
        return "两个工具都回来了：" + joined
    if mode == "toolerr":
        return "工具失败了：" + content.replace(NL, " ")[:60]
    if mode == "badargs":
        return "参数被拒绝了：" + content.replace(NL, " ")[:60]
    if mode == "shell":
        return "命令输出：" + content.replace(NL, " ").strip()[:60]
    if mode == "sleeptool":
        return "命令超时了：" + content.replace(NL, " ")[:60]
    if mode == "write":
        return "写好了：" + content.replace(NL, " ").strip()[:60]
    if mode == "plain":
        return "纯文本回答，没有用任何工具"
    return "a.txt 共 %d 行" % count_lines(content)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

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

    def _json(self, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _start_stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

    def _emit(self, data: bytes):
        self.wfile.write(b"%x" % len(data) + bytes([13, 10]) + data + bytes([13, 10]))
        self.wfile.flush()

    def _end_stream(self):
        self.wfile.write(b"0" + bytes([13, 10, 13, 10]))
        self.wfile.flush()

    # 把一次工具调用拆成若干 delta；split=True 时故意切得很碎、并且切在 JSON 中间
    def _call_deltas(self, call_id, name, arguments, split):
        if not split:
            return [{"tool_calls": [{"index": 0, "id": call_id, "type": "function",
                                     "function": {"name": name, "arguments": arguments}}]}]
        deltas = []
        # 1) id + 名字的前半 + 参数的开头
        deltas.append({"tool_calls": [{"index": 0, "id": call_id, "type": "function",
                                       "function": {"name": name[:4], "arguments": arguments[:5]}}]})
        # 2) 名字的后半 + 参数中间的一片
        deltas.append({"tool_calls": [{"index": 0,
                                       "function": {"name": name[4:], "arguments": arguments[5:12]}}]})
        # 3) 剩下一片
        deltas.append({"tool_calls": [{"index": 0, "function": {"arguments": arguments[12:]}}]})
        return deltas

    def _stream_tool_calls(self, plan, mode):
        self._start_stream()
        for obj in [chunk({"role": "assistant", "content": None})]:
            self._emit(sse(obj))
        split = (mode == "toolsplit")
        for i, (call_id, name, arguments) in enumerate(plan):
            for d in self._call_deltas(call_id, name, arguments, split):
                # split 模式下把 index 改成真实序号，模拟多工具
                if i > 0:
                    for tc in d["tool_calls"]:
                        tc["index"] = i
                self._emit(sse(chunk(d)))
        self._emit(sse(chunk({}, finish="tool_calls")))
        self._emit(sse(usage_chunk(10, 5)))
        self._emit(b"data: [DONE]" + B_NL2)
        self._end_stream()

    def _stream_answer(self, text):
        self._start_stream()
        self._emit(sse(chunk({"role": "assistant", "content": ""})))
        for piece in (text[:len(text) // 2], text[len(text) // 2:]):
            if piece:
                self._emit(sse(chunk({"content": piece})))
        self._emit(sse(chunk({}, finish="stop")))
        self._emit(sse(usage_chunk(20, 8)))
        self._emit(b"data: [DONE]" + B_NL2)
        self._end_stream()

    def _json_tool_calls(self, plan):
        self._json({"id": "s4", "object": "chat.completion", "model": "fake",
                    "choices": [{"index": 0, "finish_reason": "tool_calls",
                                 "message": {"role": "assistant", "content": None,
                                             "tool_calls": [
                                                 {"id": cid, "type": "function",
                                                  "function": {"name": n, "arguments": a}}
                                                 for cid, n, a in plan]}}],
                    "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}})

    def _json_answer(self, text):
        self._json({"id": "s4", "object": "chat.completion", "model": "fake",
                    "choices": [{"index": 0, "finish_reason": "stop",
                                 "message": {"role": "assistant", "content": text}}],
                    "usage": {"prompt_tokens": 20, "completion_tokens": 8, "total_tokens": 28}})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            body = json.loads(raw)
        except Exception:
            body = {}

        parts = [p for p in self.path.split("/") if p]
        mode = parts[0] if len(parts) > 1 else ""
        stream = bool(body.get("stream"))
        messages = body.get("messages") or []
        last_role = messages[-1].get("role", "") if messages else ""

        LAST.clear()
        LAST.update({
            "mode": mode,
            "stream": stream,
            "tools": [t.get("function", {}).get("name") for t in (body.get("tools") or [])],
            "tool_schemas": [t.get("function", {}).get("parameters") for t in (body.get("tools") or [])],
            "messages": len(messages),
            "last_role": last_role,
            "first_role": messages[0].get("role", "") if messages else "",
            "first_content": (messages[0].get("content") or "")[:200] if messages else "",
            "system_count": sum(1 for m in messages if m.get("role") == "system"),
            "roles": [m.get("role") for m in messages],
            "has_tool_calls_in_history": any(m.get("tool_calls") for m in messages),
            # S7：恢复验收看的是"模型到底收到了什么"（只追加字段，不改已有行为）。
            "messages_full": [
                {"role": m.get("role"),
                 "content": (m.get("content") or "")[:400],
                 "tool_calls": [
                     {"id": tc.get("id"), "name": (tc.get("function") or {}).get("name"),
                      "arguments": (tc.get("function") or {}).get("arguments")}
                     for tc in (m.get("tool_calls") or [])
                 ],
                 "tool_call_id": m.get("tool_call_id"),
                 "name": m.get("name")}
                for m in messages
            ],
        })

        done = (last_role == "tool")
        if mode == "loop":
            done = False  # 永远不收尾
        if mode == "plain":
            done = True   # 从不调工具：用来验证"没有工具时请求里没有 system"
        if done:
            text = final_answer(mode, messages)
            return self._stream_answer(text) if stream else self._json_answer(text)

        plan = tool_plan(mode)
        return self._stream_tool_calls(plan, mode) if stream else self._json_tool_calls(plan)


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    print("PORT=%d" % srv.server_address[1], flush=True)
    srv.serve_forever()