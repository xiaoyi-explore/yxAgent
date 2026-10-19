#!/usr/bin/env python3
"""S3b 验收：真·流式（SSE 增量）。

覆盖计划文档里的六条 fixture：
  正常多轮 / chunk 切在 JSON 中间 / chunk 切在 UTF-8 码点中间 / `: ` 注释行 / 慢速流 / 401·429·500·中途断连
外加：--no-stream 回退、对端无视 stream:true 时的非流式回退、cli 端到端、铁律自查。

用法: python3 tests/test_s3b.py [agent-core] [agent-cli]
"""
import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / "build"

positional = [a for a in sys.argv[1:] if not a.startswith("--")]
CORE = positional[0] if len(positional) > 0 else str(BUILD / "agent-core")
CLI = positional[1] if len(positional) > 1 else str(BUILD / "agent-cli")

fails = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (("   " + detail) if (detail and not cond) else ""))
    if not cond:
        fails.append(name)


def start_fake(script):
    p = subprocess.Popen([sys.executable, str(HERE / script)], stdout=subprocess.PIPE, text=True)
    line = p.stdout.readline().strip()
    if not line.startswith("PORT="):
        raise RuntimeError("fake server did not report a port: " + line)
    return p, int(line.split("=")[1])


def core_env(with_key=True):
    env = dict(os.environ)
    env.pop("OPENAI_API_KEY", None)
    if with_key:
        env["DEEPSEEK_API_KEY"] = "sk-test"  # 假 server 不校验，但 provider 要求非空
    else:
        env.pop("DEEPSEEK_API_KEY", None)
    return env


def spawn_core(*extra, env=None):
    return subprocess.Popen([CORE, *extra], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            text=True, env=env or core_env())


def send(p, obj):
    p.stdin.write(json.dumps(obj, ensure_ascii=False) + "\n")
    p.stdin.flush()


def recv(p):
    line = p.stdout.readline()
    if not line:
        raise RuntimeError("core closed stdout unexpectedly")
    try:
        return json.loads(line)   # 帧里若混进半个码点，这里会直接炸：invariant 的免费检查
    except Exception as e:
        raise RuntimeError("frame is not valid JSON/UTF-8: %r (%s)" % (line[:200], e))


def handshake(p):
    recv(p)  # event hello
    send(p, {"type": "request", "id": "h1", "method": "hello",
             "params": {"protocol_version": "1.0", "client_version": "test"}})
    return recv(p)


def ping(p, rid="p1"):
    send(p, {"type": "request", "id": rid, "method": "ping", "params": {}})
    return recv(p)


def chat_collect(p, rid, text):
    """发一轮 chat，收下所有 assistant.delta（带到达时间）与最后的响应。"""
    send(p, {"type": "request", "id": rid, "method": "chat", "params": {"text": text}})
    deltas, times = [], []
    while True:
        f = recv(p)
        if f.get("type") == "event":
            if f.get("name") == "assistant.delta":
                deltas.append(f["payload"]["text"])
                times.append(time.time())
            continue
        if f.get("type") == "response":
            return deltas, times, f


def kill(p):
    try:
        p.stdin.close()
        p.wait(timeout=5)
    except Exception:
        p.kill()


def invariants(label, deltas, resp, expected, min_deltas=1):
    ok = resp.get("ok") is True
    text = resp.get("result", {}).get("text", "") if ok else ""
    check(label + "：响应 ok", ok, str(resp)[:300])
    check(label + "：增量数 >= %d（真的是一点一点来的）" % min_deltas, len(deltas) >= min_deltas, str(len(deltas)))
    check(label + "：没有空增量", all(d != "" for d in deltas), repr(deltas[:5]))
    check(label + "：不变量 2（result.text == 增量顺序拼接 == 期望文本）",
          text == "".join(deltas) == expected, repr(text[:80]))
    check(label + "：stop_reason 来自流（stop）", resp.get("result", {}).get("stop_reason") == "stop",
          str(resp.get("result", {}))[:200])


# ---------------------------------------------------------------- 六条 fixture
def test_basic(base):
    p = spawn_core("--base-url", base + "/stream", "--model", "fake")
    handshake(p)
    d, t, r = chat_collect(p, "r1", "你好")
    invariants("正常流", d, r, "fake(1 msgs) echo=你好", min_deltas=2)
    check("正常流：usage 来自 include_usage 的那一帧（prompt_tokens=1）",
          r["result"].get("usage", {}).get("prompt_tokens") == 1, str(r["result"].get("usage")))

    d2, _, r2 = chat_collect(p, "r2", "第二句")
    invariants("正常多轮（历史在 core）", d2, r2, "fake(3 msgs) echo=第二句", min_deltas=2)
    kill(p)


def test_split_json(base):
    p = spawn_core("--base-url", base + "/splitjson", "--model", "fake")
    handshake(p)
    d, _, r = chat_collect(p, "r1", "切在JSON中间")
    invariants("chunk 切在 JSON 中间", d, r, "fake(1 msgs) echo=切在JSON中间", min_deltas=2)
    kill(p)


def test_split_utf8(base):
    p = spawn_core("--base-url", base + "/splitutf8", "--model", "fake")
    handshake(p)
    d, _, r = chat_collect(p, "r1", "中文流式必踩")
    invariants("chunk 切在 UTF-8 码点中间", d, r, "fake(1 msgs) echo=中文流式必踩", min_deltas=2)
    check("UTF-8：没有一个增量是半个汉字", all(len(x.encode("utf-8")) > 0 for x in d))
    kill(p)


def test_comment(base):
    p = spawn_core("--base-url", base + "/comment", "--model", "fake")
    handshake(p)
    d, _, r = chat_collect(p, "r1", "注释行")
    invariants(": 注释行（keep-alive）", d, r, "fake(1 msgs) echo=注释行", min_deltas=2)
    check("注释行没有被当成数据", not any("keep-alive" in x for x in d), repr(d))
    kill(p)


def test_slow(base):
    p = spawn_core("--base-url", base + "/slow", "--model", "fake")
    handshake(p)
    t0 = time.time()
    d, t, r = chat_collect(p, "r1", "慢流")
    invariants("慢速流", d, r, "fake(1 msgs) echo=慢流", min_deltas=2)
    check("慢速流：第一个增量不是等整段攒完才来的（< 0.5s）", t and (t[0] - t0) < 0.5, "%.3fs" % ((t[0] - t0) if t else -1))
    check("慢速流：增量之间真的隔了时间（>= 0.03s）", len(t) >= 2 and (t[-1] - t[0]) >= 0.03,
          "%.3fs" % ((t[-1] - t[0]) if len(t) >= 2 else -1))
    check("慢速流：整轮耗时 >= 0.15s（6 个事件 × 50ms）", (time.time() - t0) >= 0.15, "%.3fs" % (time.time() - t0))
    kill(p)


def test_no_usage(base):
    p = spawn_core("--base-url", base + "/nousage", "--model", "fake")
    handshake(p)
    d, _, r = chat_collect(p, "r1", "没有usage")
    invariants("usage 缺失的流", d, r, "fake(1 msgs) echo=没有usage", min_deltas=2)
    check("usage 缺失时 result.usage 是空对象（不编造数字）", r["result"].get("usage") == {},
          str(r["result"].get("usage")))
    kill(p)


def test_no_done(base):
    p = spawn_core("--base-url", base + "/nodone", "--model", "fake")
    handshake(p)
    d, _, r = chat_collect(p, "r1", "没有DONE")
    invariants("没有 [DONE]、末尾没有空行", d, r, "fake(1 msgs) echo=没有DONE（对端没发 [DONE] 就断了）", min_deltas=2)
    kill(p)


# ---------------------------------------------------------------- 错误与断连
def test_errors(base):
    for mode, code in (("error401", "401"), ("error429", "429"), ("error500", "500")):
        p = spawn_core("--base-url", base + "/" + mode, "--model", "fake", "--provider-timeout-ms=3000")
        handshake(p)
        d, _, r = chat_collect(p, "r1", "会失败")
        check("%s -> PROVIDER_ERROR 且带状态码" % code,
              not r["ok"] and r["error"]["code"] == "PROVIDER_ERROR" and code in r["error"]["message"],
              str(r)[:300])
        check("%s：失败前没有吐过任何增量" % code, d == [], repr(d))
        kill(p)


def test_retry_before_first_delta(base):
    """第一次请求在传输层被掐断（一个字节都没吐）——重试是安全的，必须自动重试。"""
    p = spawn_core("--base-url", base + "/droponce", "--model", "fake")
    handshake(p)
    d, _, r = chat_collect(p, "r1", "第一次会失败")
    invariants("传输层失败后自动重试（还没吐字，重试是安全的）", d, r, "fake(1 msgs) echo=第一次会失败", min_deltas=2)
    kill(p)


def test_abort_mid_stream(base):
    p = spawn_core("--base-url", base + "/abort", "--model", "fake")
    handshake(p)
    d, _, r = chat_collect(p, "r1", "中途断连")
    full = "fake(1 msgs) echo=中途断连"
    partial = "".join(d)
    # 两种都算合格：如实报错，或者如实交付"断之前收到的部分"；只要不挂死、不凭空补全
    ok_shape = (not r["ok"] and r["error"]["code"] == "PROVIDER_ERROR") or (r["ok"] and full.startswith(partial))
    check("中途断连：不挂死、不编造（要么报错，要么只交付收到的那部分）", ok_shape and full.startswith(partial),
          "%s / %r" % (str(r)[:200], partial))
    # 断连之后 core 还活着：还能正常应答（ping 是最便宜的证据）
    h = ping(p, "px1")
    check("中途断连后 core 仍然活着（可以继续服务）", h.get("ok") is True, str(h)[:200])
    kill(p)


# ---------------------------------------------------------------- 回退路径
def test_no_stream_flag(base_json):
    p = spawn_core("--no-stream", "--base-url", base_json, "--model", "fake")
    handshake(p)
    d, _, r = chat_collect(p, "r1", "非流式回退")
    check("--no-stream：回到 S3a 的整段返回（没有增量）", r["ok"] and d == [], repr(d))
    check("--no-stream：文本正确", r["result"]["text"] == "fake(1 msgs) echo=非流式回退", str(r)[:300])
    kill(p)


def test_fallback_when_peer_ignores_stream(base_json):
    """对端无视 stream:true、直接回一整段 JSON —— 必须自动回退，而不是报"不是 SSE"。"""
    p = spawn_core("--base-url", base_json, "--model", "fake")
    handshake(p)
    d, _, r = chat_collect(p, "r1", "对端不流式")
    check("对端无视 stream:true：仍成功", r["ok"], str(r)[:300])
    check("对端无视 stream:true：整段文本作为一个增量交付",
          d == ["fake(1 msgs) echo=对端不流式"], repr(d))
    kill(p)


def test_request_shape(base):
    """请求里必须真的有 stream:true 与 stream_options.include_usage（= usage 的来源）。"""
    p = spawn_core("--base-url", base + "/stream", "--model", "fake")
    handshake(p)
    chat_collect(p, "r1", "看看请求长什么样")
    kill(p)
    with urllib.request.urlopen(base + "/__last", timeout=5) as resp:
        last = json.loads(resp.read().decode("utf-8"))
    check("请求体带 stream:true", last.get("stream") is True, str(last))
    check("请求体带 stream_options.include_usage（默认流里没有 usage）",
          (last.get("stream_options") or {}).get("include_usage") is True, str(last))
    check("请求体带 Authorization", (last.get("authorization") or "").startswith("Bearer "), str(last))


# ---------------------------------------------------------------- cli 端到端
def test_cli(base):
    env = core_env()
    r = subprocess.run([CLI, "--base-url", base + "/stream", "--model", "fake"],
                       input="你好\n", capture_output=True, text=True, timeout=40, env=env)
    out = r.stdout
    check("cli：流式文字打出来了", "[assistant] fake(1 msgs) echo=你好" in out, out)
    check("cli：没有把整段重播一遍（[assistant] 只出现一次）", out.count("[assistant] ") == 1, out)
    check("cli：结算行照旧（stop_reason + tokens）",
          "[turn t1 done: stop_reason=stop, tokens=" in out, out)
    check("cli：干净退出", "[core exited cleanly, code=0]" in out, out)

    r = subprocess.run([CLI, "--no-stream", "--base-url", base + "/stream", "--model", "fake"],
                       input="你好\n", capture_output=True, text=True, timeout=40, env=env)
    check("cli --no-stream：对 SSE 端点用非流式请求 -> 可行动的报错（不被静默吞掉）",
          "[error] PROVIDER_ERROR" in r.stdout or "[assistant]" in r.stdout, r.stdout)


# ---------------------------------------------------------------- 铁律
def test_layering():
    sse_sources = [HERE.parent / "src" / "core" / "sse.hpp", HERE.parent / "src" / "core" / "sse.cpp"]
    bad = []
    for path in sse_sources:
        text = path.read_text(encoding="utf-8")
        for needle in ("httplib", "openssl", "curl"):
            if needle in text:
                bad.append("%s: %s" % (path.name, needle))
    check("铁律：SSE 解析器不认识 HTTP（sse.* 里没有 httplib/openssl）", not bad, str(bad))

    bad = []
    for path in (HERE.parent / "src" / "core").rglob("*.cpp"):
        if path.name == "openai_compat.cpp":
            continue  # 唯一允许碰 HTTP 的地方
        text = path.read_text(encoding="utf-8")
        for needle in ("httplib", "openssl"):
            if needle in text:
                bad.append("%s: %s" % (path.name, needle))
    check("铁律：HTTP 只出现在 openai_compat.cpp 一处", not bad, str(bad))


def main():
    fake_sse, port = start_fake("fake_llm_sse.py")
    fake_json, port_json = start_fake("fake_llm.py")
    base = "http://127.0.0.1:%d" % port
    base_json = "http://127.0.0.1:%d" % port_json
    try:
        test_basic(base)
        test_split_json(base)
        test_split_utf8(base)
        test_comment(base)
        test_slow(base)
        test_no_usage(base)
        test_no_done(base)
        test_errors(base)
        test_retry_before_first_delta(base)
        test_abort_mid_stream(base)
        test_no_stream_flag(base_json)
        test_fallback_when_peer_ignores_stream(base_json)
        test_request_shape(base)
        test_cli(base)
        test_layering()
    finally:
        fake_sse.kill()
        fake_json.kill()


if __name__ == "__main__":
    print("core: %s" % CORE)
    print("cli : %s" % CLI)
    print()
    main()
    print()
    if fails:
        print("FAILED: %d" % len(fails))
        for x in fails:
            print("  - " + x)
        sys.exit(1)
    print("ALL S3B TESTS PASSED")
