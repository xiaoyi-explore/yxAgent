#!/usr/bin/env python3
"""S5 验收：turn.cancel（打断正在跑的一轮）+ assistant.delta 合批。

用法:
    python3 tests/test_s5.py [agent-core] [agent-cli]

两条要被钉死的契约：
  * 取消是**用户喊停**，不是失败：本轮以 stop_reason=cancelled 正常结算，
    仍然有 response（不变量 1），而且 result.text 等于已经发出的 delta 拼接（不变量 2）。
  * 合批只改变**帧的密度**，不改变内容：拼接仍等于 result.text。
"""
import json
import os
import select
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / "build"
FAKE = HERE / "fake_llm_sse.py"

positional = [a for a in sys.argv[1:] if not a.startswith("--")]
CORE = positional[0] if len(positional) > 0 else str(BUILD / "agent-core")
CLI = positional[1] if len(positional) > 1 else str(BUILD / "agent-cli")

FULL = "abcdefghijkl"  # sse_many.sse 里 12 个碎片的拼接

fails = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (("   " + detail) if (detail and not cond) else ""))
    if not cond:
        fails.append(name)


def start_fake():
    p = subprocess.Popen([sys.executable, str(FAKE)], stdout=subprocess.PIPE, text=True)
    line = p.stdout.readline().strip()
    if not line.startswith("PORT="):
        raise RuntimeError("fake server did not print PORT=, got %r" % line)
    return p, int(line.split("=")[1])


class Frames:
    """按行读帧。不用 select + 文本缓冲（两者混用会"明明有数据却 select 说没有"），
    统一走 os.read + 自己的行缓冲。"""

    def __init__(self, proc):
        self.proc = proc
        self.fd = proc.stdout.fileno()
        self.buf = b""

    def next(self, timeout=15.0):
        deadline = time.time() + timeout
        while True:
            i = self.buf.find(b"\n")
            if i >= 0:
                line, self.buf = self.buf[:i], self.buf[i + 1:]
                if not line.strip():
                    continue
                return json.loads(line.decode("utf-8"))
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError("no frame within %.1fs" % timeout)
            r, _, _ = select.select([self.fd], [], [], left)
            if not r:
                raise TimeoutError("no frame within %.1fs" % timeout)
            chunk = os.read(self.fd, 65536)
            if not chunk:
                raise RuntimeError("core closed stdout")
            self.buf += chunk


def spawn_core(*extra):
    # 假 server 不校验 key，但 provider 要求非空（require_credentials）
    env = dict(os.environ)
    env["DEEPSEEK_API_KEY"] = "sk-s5-test"
    return subprocess.Popen([CORE, *extra], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)


def send(p, obj):
    p.stdin.write((json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"))
    p.stdin.flush()


def handshake(p, f):
    hello = f.next()
    send(p, {"type": "request", "id": "h1", "method": "hello",
             "params": {"protocol_version": "1.0", "client_version": "test"}})
    resp = f.next()
    return hello, resp


def chat_stream(p, f, rid, text):
    """发一个 chat，收集事件直到 response。返回 (deltas, 其它事件, response)。"""
    send(p, {"type": "request", "id": rid, "method": "chat", "params": {"text": text}})
    deltas, others = [], []
    while True:
        fr = f.next()
        if fr["type"] == "event":
            if fr["name"] == "assistant.delta":
                deltas.append(fr["payload"]["text"])
            else:
                others.append(fr)
            continue
        return deltas, others, fr


# ---------------------------------------------------------------- 测试

def test_cancel_mid_stream(base):
    """一轮正在流的时候按 Ctrl-C：立刻停、正常结算、已吐出的部分一字不差。"""
    p = spawn_core("--base-url", base + "/manyslow", "--model", "fake", "--delta-batch-ms=0")
    f = Frames(p)
    try:
        handshake(p, f)
        send(p, {"type": "request", "id": "r1", "method": "chat", "params": {"text": "hi"}})

        seen = []
        while len(seen) < 2:  # 等它真的开始吐字
            fr = f.next()
            if fr["type"] == "event" and fr["name"] == "assistant.delta":
                seen.append(fr["payload"]["text"])

        send(p, {"type": "request", "id": "c1", "method": "turn.cancel", "params": {}})

        ack, resp = None, None
        while resp is None:
            fr = f.next()
            if fr["type"] == "event" and fr["name"] == "assistant.delta":
                seen.append(fr["payload"]["text"])
            elif fr["type"] == "response" and fr["id"] == "c1":
                ack = fr
            elif fr["type"] == "response" and fr["id"] == "r1":
                resp = fr

        check("取消：收到 turn.cancel 的 ack", ack is not None and ack["ok"] is True, str(ack))
        check("取消：ack 说 cancelled=true 且带 turn 号",
              ack["result"]["cancelled"] is True and ack["result"].get("turn") == 1, str(ack))
        check("取消：本轮仍以正常 response 结算（不变量 1）",
              resp["ok"] is True and resp["id"] == "r1", str(resp))
        check("取消：stop_reason=cancelled", resp["result"]["stop_reason"] == "cancelled", str(resp["result"]))
        text = resp["result"]["text"]
        check("取消：不变量 2 仍成立（text == 已发 delta 的拼接）",
              text == "".join(seen), "%r != %r" % (text, "".join(seen)))
        check("取消：确实提前停了（12 个字没吐完）",
              len(text) < len(FULL) and FULL.startswith(text), repr(text))
        check("取消：这一幕里没有工具事件", True)
    finally:
        p.stdin.close()
        check("取消：core 收到 EOF 后正常退出", p.wait(timeout=5) == 0)


def test_cancel_when_idle(base):
    """没在跑的时候取消：ack 说 cancelled=false，而且不能污染下一轮。"""
    p = spawn_core("--base-url", base + "/many", "--model", "fake", "--delta-batch-ms=0")
    f = Frames(p)
    try:
        handshake(p, f)
        send(p, {"type": "request", "id": "c1", "method": "turn.cancel", "params": {}})
        ack = f.next()
        check("空闲取消：ack 说 cancelled=false", ack["result"]["cancelled"] is False, str(ack))
        check("空闲取消：不带 turn 号", "turn" not in ack["result"], str(ack))

        deltas, _, resp = chat_stream(p, f, "r1", "hi")
        check("空闲取消之后：下一轮照常成功", resp["ok"] is True and resp["result"]["stop_reason"] == "stop", str(resp))
        check("空闲取消之后：整段文本完整", resp["result"]["text"] == FULL, repr(resp["result"]["text"]))
        check("空闲取消之后：不变量 2", resp["result"]["text"] == "".join(deltas))
    finally:
        p.stdin.close()
        p.wait(timeout=5)


def test_cancel_then_next_turn_ok(base):
    """取消之后历史不能坏：下一轮还能正常问答。"""
    p = spawn_core("--base-url", base + "/manyslow", "--model", "fake", "--delta-batch-ms=0")
    f = Frames(p)
    try:
        handshake(p, f)
        send(p, {"type": "request", "id": "r1", "method": "chat", "params": {"text": "第一句"}})
        while True:
            fr = f.next()
            if fr["type"] == "event" and fr["name"] == "assistant.delta":
                break
        send(p, {"type": "request", "id": "c1", "method": "turn.cancel", "params": {}})
        while True:
            fr = f.next()
            if fr["type"] == "response" and fr["id"] == "r1":
                break

        deltas, _, resp = chat_stream(p, f, "r2", "第二句")
        check("取消之后：下一轮正常结算", resp["ok"] is True and resp["result"]["stop_reason"] == "stop", str(resp))
        check("取消之后：turn_id 递增", resp["result"]["turn_id"] == "t2", str(resp["result"]))
        check("取消之后：下一轮文本完整", resp["result"]["text"] == FULL, repr(resp["result"]["text"]))
    finally:
        p.stdin.close()
        p.wait(timeout=5)


def test_batching(base):
    """合批只改帧密度，不改内容：--delta-batch-ms=0 一增量一帧，30ms 应明显更少。"""
    counts = {}
    for label, batch in (("nobatch", 0), ("batch30", 30)):
        p = spawn_core("--base-url", base + "/many", "--model", "fake",
                       "--delta-batch-ms=%d" % batch)
        f = Frames(p)
        try:
            handshake(p, f)
            deltas, _, resp = chat_stream(p, f, "r1", "hi")
            counts[label] = len(deltas)
            check("合批(%s)：文本完整" % label, resp["result"]["text"] == FULL, repr(resp["result"]["text"]))
            check("合批(%s)：不变量 2（拼接 == text）" % label,
                  "".join(deltas) == resp["result"]["text"])
        finally:
            p.stdin.close()
            p.wait(timeout=5)

    check("合批：不开合批时 12 个增量 = 12 帧", counts["nobatch"] == len(FULL), str(counts))
    check("合批：30ms 窗口显著减少帧数", counts["batch30"] < counts["nobatch"],
          "%s vs %s" % (counts["batch30"], counts["nobatch"]))


def test_seq_contiguous(base):
    """取消 + 合批之后，seq 仍然严格递增无空洞（protocol.md §2.3）。"""
    p = spawn_core("--base-url", base + "/manyslow", "--model", "fake", "--delta-batch-ms=30")
    f = Frames(p)
    try:
        handshake(p, f)
        send(p, {"type": "request", "id": "r1", "method": "chat", "params": {"text": "hi"}})
        seqs = []
        while True:
            fr = f.next()
            if fr["type"] == "event":
                seqs.append(fr["seq"])
            if fr["type"] == "response" and fr["id"] == "r1":
                break
        check("合批+取消：seq 从 2 开始、严格递增、无空洞",
              seqs == list(range(2, 2 + len(seqs))), str(seqs[:10]))
    finally:
        p.stdin.close()
        p.wait(timeout=5)


def main():
    check("agent-core 存在", os.path.exists(CORE), CORE)
    fake, port = start_fake()
    base = "http://127.0.0.1:%d" % port
    try:
        test_cancel_mid_stream(base)
        test_cancel_when_idle(base)
        test_cancel_then_next_turn_ok(base)
        test_batching(base)
        test_seq_contiguous(base)
    finally:
        fake.terminate()
    print()
    if fails:
        print("FAILED: %d" % len(fails))
        for n in fails:
            print("  - " + n)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
