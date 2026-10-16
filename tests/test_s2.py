#!/usr/bin/env python3
"""S2 验收：事件流（demo.chat -> assistant.delta* -> response）/ --log-raw / --embedded 等價。

用法:
    python3 tests/test_s2.py [agent-core] [agent-cli]
    python3 tests/test_s2.py --regen        # 重新生成 fixtures/demo_chat.ndjson（模型模板改动时用）
"""
import json
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / "build"

positional = [a for a in sys.argv[1:] if not a.startswith("--")]
CORE = positional[0] if len(positional) > 0 else str(BUILD / "agent-core")
CLI = positional[1] if len(positional) > 1 else str(BUILD / "agent-cli")
FIXTURE = HERE / "fixtures" / "demo_chat.ndjson"

fails = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (("   " + detail) if (detail and not cond) else ""))
    if not cond:
        fails.append(name)


def spawn_core(*extra):
    return subprocess.Popen([CORE, "--demo-delay-ms=0", *extra],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)


def send(p, obj):
    p.stdin.write(json.dumps(obj, ensure_ascii=False) + "\n")
    p.stdin.flush()


def recv(p):
    line = p.stdout.readline()
    if not line:
        raise RuntimeError("core closed stdout unexpectedly")
    return json.loads(line)


def handshake(p):
    ev = recv(p)
    send(p, {"type": "request", "id": "h1", "method": "hello",
             "params": {"protocol_version": "1.0", "client_version": "test"}})
    return ev, recv(p)


def chat(p, rid, text):
    """发一个 demo.chat，返回 (事件帧列表, 响应帧)。"""
    send(p, {"type": "request", "id": rid, "method": "demo.chat", "params": {"text": text}})
    events = []
    while True:
        f = recv(p)
        if f["type"] == "event":
            events.append(f)
            continue
        return events, f


def capture_stream(text="你好"):
    """完整捕获一次 demo.chat 的帧序列（含请求帧），用于 golden fixture。"""
    p = spawn_core()
    recv(p)  # event hello（含 pid，不进 fixture）
    frames = [{"type": "request", "id": "r1", "method": "demo.chat", "params": {"text": text}}]
    send(p, frames[0])
    while True:
        f = recv(p)
        frames.append(f)
        if f["type"] == "response":
            break
    p.stdin.close()
    p.wait(timeout=5)
    return frames


# ---------------------------------------------------------------- 测试

def test_stream_shape():
    p = spawn_core()
    handshake(p)
    events, resp = chat(p, "r1", "你好")

    check("demo.chat: 吐出了若干 delta", len(events) >= 5, f"n={len(events)}")
    check("demo.chat: 中间帧全是 assistant.delta",
          all(e["name"] == "assistant.delta" for e in events))
    check("demo.chat: response 最后结算并匹配 id",
          resp["type"] == "response" and resp["id"] == "r1" and resp["ok"] is True, str(resp))
    check("demo.chat: turn_id = t1", resp["result"].get("turn_id") == "t1", str(resp["result"]))
    check("demo.chat: stop_reason = stop", resp["result"].get("stop_reason") == "stop")

    joined = "".join(e["payload"]["text"] for e in events)
    check("不变量: result.text == 所有 delta.text 顺序拼接",
          joined == resp["result"]["text"], f"{joined!r} != {resp['result']['text']!r}")
    check("demo.chat: 回复里带回了用户输入", "你好" in resp["result"]["text"])
    check("每个 delta 恰好一个 UTF-8 码点",
          all(len(e["payload"]["text"]) == 1 for e in events),
          str([e["payload"]["text"] for e in events if len(e["payload"]["text"]) != 1][:5]))

    seqs = [e["seq"] for e in events]
    check("seq 严格递增且无空洞", seqs == list(range(seqs[0], seqs[0] + len(seqs))), str(seqs[:8]))
    check("hello 占用 seq=1，delta 从 2 开始", seqs[0] == 2, str(seqs[:3]))

    send(p, {"type": "request", "id": "r2", "method": "shutdown", "params": {}})
    bye = recv(p)
    check("shutdown 被确认且退出码 0", bye["ok"] is True and p.wait(timeout=5) == 0)


def test_multiturn():
    p = spawn_core()
    handshake(p)
    e1, r1 = chat(p, "r1", "第一句")
    e2, r2 = chat(p, "r2", "第二句")
    check("多轮: turn_id 递增", (r1["result"]["turn_id"], r2["result"]["turn_id"]) == ("t1", "t2"))
    check("多轮: 第二轮回复里的轮次正确", "第 2 句" in r2["result"]["text"])
    check("多轮: seq 跨请求连续（不重置、不空洞）",
          e2[0]["seq"] == e1[-1]["seq"] + 1,
          "%d -> %d" % (e1[-1]["seq"], e2[0]["seq"]))
    p.stdin.close()
    p.wait(timeout=5)


def test_log_raw():
    with tempfile.TemporaryDirectory() as td:
        log = Path(td) / "s2.log"
        p = spawn_core("--log-raw", str(log))
        handshake(p)
        chat(p, "r1", "抓包")
        send(p, {"type": "request", "id": "r2", "method": "shutdown", "params": {}})
        recv(p)
        p.wait(timeout=5)

        lines = log.read_text(encoding="utf-8").strip().split("\n")
        check("log-raw: 每行都带方向前缀（--> / <--）",
              all(l.startswith("--> ") or l.startswith("<-- ") for l in lines), str(lines[:2]))
        dirs = [l[:3] for l in lines]
        check("log-raw: 双向都记到了", "-->" in dirs and "<--" in dirs)
        frames = [json.loads(l[4:]) for l in lines]
        check("log-raw: 去掉前缀后每行都是合法 JSON 帧",
              all(isinstance(f, dict) and "type" in f for f in frames))
        check("log-raw: 第一帧是 core 主动发的 hello 事件",
              frames[0].get("name") == "hello", str(frames[0]))
        check("log-raw: 最后一帧是 shutdown 的响应",
              frames[-1]["type"] == "response" and frames[-1]["result"] == {"will_exit": True}, str(frames[-1]))


def test_s1_regressions():
    p = spawn_core()
    handshake(p)
    send(p, {"type": "request", "id": "r1", "method": "echo", "params": {"line": "hi"}})
    check("回归: echo 仍可用", recv(p)["result"] == {"line": "hi"})
    send(p, {"type": "request", "id": "r2", "method": "nope", "params": {}})
    check("回归: 未知方法 -> UNKNOWN_METHOD", recv(p)["error"]["code"] == "UNKNOWN_METHOD")
    p.stdin.write("not json\n")
    p.stdin.flush()
    r = recv(p)
    check("回归: 垃圾帧 -> PARSE_ERROR(id=null)",
          r["error"]["code"] == "PARSE_ERROR" and r["id"] is None, str(r))
    send(p, {"type": "event", "seq": 99, "name": "bogus", "payload": {}})
    send(p, {"type": "request", "id": "r3", "method": "echo", "params": {"line": "alive"}})
    r = recv(p)
    check("回归: 未知事件被忽略、后续请求照常", r["id"] == "r3" and r["result"]["line"] == "alive")
    p.stdin.close()
    p.wait(timeout=5)

    p2 = spawn_core()
    recv(p2)
    send(p2, {"type": "request", "id": "h", "method": "hello",
              "params": {"protocol_version": "2.0"}})
    r = recv(p2)
    check("回归: major 不兼容 -> PROTOCOL_VERSION_MISMATCH",
          r["error"]["code"] == "PROTOCOL_VERSION_MISMATCH")
    check("回归: 不兼容退出码 2", p2.wait(timeout=5) == 2)


def run_cli(extra):
    r = subprocess.run([CLI, "--demo-delay-ms=0", *extra],
                       input="你好\n第二句\n", capture_output=True, text=True, timeout=30)
    return r.stdout


def test_embedded_equivalence():
    pipe_out = run_cli([])
    emb_out = run_cli(["--embedded"])
    norm = lambda s: re.sub(r"pid=\d+", "pid=X", s)
    check("--embedded 与管道模式输出一致（pid 除外）", norm(pipe_out) == norm(emb_out),
          "\n--- pipe ---\n" + norm(pipe_out) + "\n--- embedded ---\n" + norm(emb_out))
    check("cli 渲染了打字机前缀 [assistant]", "[assistant] " in pipe_out)
    check("cli 渲染了 turn 结算行",
          re.search(r"\[turn t1 done: \d+ deltas, stop_reason=stop\]", pipe_out) is not None, pipe_out)
    check("两轮都被处理（t2 结算行存在）", "[turn t2 done:" in pipe_out)


def test_streaming_is_incremental():
    delay = 15
    p = subprocess.Popen([CORE, "--demo-delay-ms=%d" % delay],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    recv(p)
    send(p, {"type": "request", "id": "h1", "method": "hello", "params": {"protocol_version": "1.0"}})
    recv(p)
    t0 = time.time()
    send(p, {"type": "request", "id": "r1", "method": "demo.chat", "params": {"text": "流式"}})
    first = None
    n = 0
    while True:
        f = recv(p)
        if f["type"] == "event":
            n += 1
            if first is None:
                first = time.time() - t0
            continue
        total = time.time() - t0
        break
    check("流式: 第一帧远早于最后一帧（不是攒完一次吐）",
          first is not None and first < total * 0.5, f"first={first:.3f}s total={total:.3f}s n={n}")
    check("流式: 总耗时与逐帧 delay 相称", total >= n * delay / 1000.0 * 0.4,
          f"total={total:.3f}s n={n} delay={delay}ms")
    p.stdin.close()
    p.wait(timeout=5)


def test_golden_fixture():
    expected = [json.loads(l) for l in FIXTURE.read_text(encoding="utf-8").strip().split("\n")]
    actual = capture_stream(expected[0]["params"]["text"])
    check("golden: demo.chat 的完整帧序列可逐帧复现", actual == expected,
          f"len {len(actual)} vs {len(expected)}")


# ---------------------------------------------------------------- 入口

if "--regen" in sys.argv:
    frames = capture_stream("你好")
    FIXTURE.parent.mkdir(parents=True, exist_ok=True)
    FIXTURE.write_text("\n".join(json.dumps(f, ensure_ascii=False) for f in frames) + "\n",
                       encoding="utf-8")
    print(f"regenerated {FIXTURE} ({len(frames)} frames)")
    sys.exit(0)

print(f"core: {CORE}")
print(f"cli : {CLI}")
print()
test_stream_shape()
test_multiturn()
test_log_raw()
test_s1_regressions()
test_embedded_equivalence()
test_streaming_is_incremental()
test_golden_fixture()
print()
if fails:
    print("FAILED: %d" % len(fails))
    for f in fails:
        print("  - " + f)
    sys.exit(1)
print("ALL S2 TESTS PASSED")
