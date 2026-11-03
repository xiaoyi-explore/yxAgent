#!/usr/bin/env python3
"""S6 验收：上下文预算与压缩（端到端）。

用法:
    python3 tests/test_s6.py [agent-core] [agent-cli]

算法本身由 tests/context_test.cpp（C++ 单元测试）穷举；这里只验**接线**：
  * 超预算时真的发出 context.compacted 事件，字段齐、seq 连续；
  * 压缩后发给模型的 messages 确实变小，而且 system 还在第 0 条、摘要里带着早期事实；
  * --max-context-tokens=0 时完全不压；
  * cli 不再把"不知道 usage"显示成 tokens=0/0/0。
"""
import json
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / "build"
FAKE = HERE / "fake_llm_sse.py"

positional = [a for a in sys.argv[1:] if not a.startswith("--")]
CORE = positional[0] if len(positional) > 0 else str(BUILD / "agent-core")
CLI = positional[1] if len(positional) > 1 else str(BUILD / "agent-cli")

TEXT = "abcdefghijkl"   # sse_many.sse 的 12 个碎片

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


def core_env():
    env = dict(os.environ)
    env.pop("OPENAI_API_KEY", None)
    env["DEEPSEEK_API_KEY"] = "sk-s6-test"
    return env


def spawn_core(*extra):
    return subprocess.Popen([CORE, *extra], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            text=True, env=core_env())


def send(p, obj):
    p.stdin.write(json.dumps(obj, ensure_ascii=False) + "\n")
    p.stdin.flush()


def recv(p):
    line = p.stdout.readline()
    if not line:
        raise RuntimeError("core closed stdout unexpectedly")
    return json.loads(line)


def handshake(p):
    recv(p)  # event hello
    send(p, {"type": "request", "id": "h1", "method": "hello",
             "params": {"protocol_version": "1.0", "client_version": "test"}})
    return recv(p)


def chat_turn(p, rid, text):
    """返回 (事件列表, 响应)。"""
    send(p, {"type": "request", "id": rid, "method": "chat", "params": {"text": text}})
    events = []
    while True:
        f = recv(p)
        if f["type"] == "event":
            events.append(f)
            continue
        return events, f


def last_request(port):
    with urllib.request.urlopen("http://127.0.0.1:%d/__last" % port, timeout=5) as r:
        return json.load(r)


# ---------------------------------------------------------------- 测试

def test_compaction_event(base):
    """超预算 -> 发 context.compacted，字段齐，seq 仍无空洞。"""
    p = spawn_core("--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=60", "--keep-recent-turns=1")
    try:
        handshake(p)
        seen = []
        compacted = []
        seqs = []
        for i in range(8):
            # 用户消息故意长一点：压缩在短对话上是赚不回来的（ContextManager 有"不许变大"的硬保证）
            events, resp = chat_turn(p, "r%d" % i, "问题%d：" % i + "x" * 300)
            check("第 %d 轮正常结算" % (i + 1), resp["ok"] is True, str(resp))
            for ev in events:
                seqs.append(ev["seq"])
                if ev["name"] == "context.compacted":
                    compacted.append(ev)
                seen.append(ev["name"])
        check("超预算后出现了 context.compacted", len(compacted) >= 1, str(seen))
        if compacted:
            c = compacted[0]["payload"]
            check("事件带 before/after 且真的变小", c["after_tokens"] < c["before_tokens"],
                  "%s -> %s" % (c["before_tokens"], c["after_tokens"]))
            check("事件带 budget", c["budget"] == 60, str(c["budget"]))
            check("事件说折叠了轮", c["summarized_turns"] >= 1, str(c["summarized_turns"]))
            check("事件带 turn_id / step", "turn_id" in c and "step" in c, str(c))
        check("seq 严格递增无空洞（含新事件）", seqs == list(range(seqs[0], seqs[0] + len(seqs))),
              str(seqs[:12]))
    finally:
        p.stdin.close()
        p.wait(timeout=5)


def test_compacted_request_shape(base, port):
    """压缩后的请求：system 还在第 0 条、摘要带着早期事实、最近轮保留。"""
    fact = "早期事实：代号是蓝鲸"
    p = spawn_core("--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=60", "--keep-recent-turns=1",
                   "--system", "你是测试助手")
    try:
        handshake(p)
        chat_turn(p, "r0", fact)
        for i in range(1, 7):
            chat_turn(p, "r%d" % i, "第%d句：" % i + "y" * 300)
        info = last_request(port)
        msgs = info.get("messages_full") or []
        roles = [m["role"] for m in msgs]
        check("压缩后请求里仍有 system", roles and roles[0] == "system", str(roles))
        check("system 内容是原样（不是摘要）", "你是测试助手" in msgs[0]["content"], msgs[0]["content"][:40])
        summary = [m for m in msgs if m["role"] == "user" and "[压缩]" in m["content"]]
        check("出现了压缩摘要消息", len(summary) == 1, str(roles))
        check("摘要里带上了早期事实（压缩后仍能被模型看到）",
              summary and fact in summary[0]["content"],
              summary[0]["content"][:160] if summary else "no summary")
        check("压缩后消息数明显变少", len(msgs) <= 5, "%d 条: %s" % (len(msgs), roles))
        check("最后一条是当前的 user（最近轮保留）", roles[-1] == "user", str(roles))
        check("没有孤儿 tool 消息", all(
            m["role"] != "tool" for m in msgs), str(roles))
    finally:
        p.stdin.close()
        p.wait(timeout=5)


def test_budget_zero_disables(base):
    """--max-context-tokens=0：完全不压缩（给"我要看原始行为"用）。"""
    p = spawn_core("--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        names = []
        for i in range(6):
            events, resp = chat_turn(p, "r%d" % i, "第%d句，内容长一点点" % i)
            check("关压缩：第 %d 轮仍正常" % (i + 1), resp["ok"] is True)
            names += [e["name"] for e in events]
        check("关压缩：一个 context.compacted 都没有", "context.compacted" not in names, str(names))
    finally:
        p.stdin.close()
        p.wait(timeout=5)


def test_cli_usage_unknown(base):
    """对端不给 usage 时，cli 不该打印 tokens=0/0/0（把"未知"显示成"零"）。"""
    r = subprocess.run([CLI, "--demo-delay-ms=0", "--max-context-tokens=0",
                        "--base-url", base + "/nousage", "--model", "fake"],
                       input="你好\n", capture_output=True, text=True, timeout=40, env=core_env())
    check("cli 在 usage 缺失时不编造 tokens 数字", "tokens=" not in r.stdout, r.stdout[-300:])
    check("cli 仍然正常结算", "stop_reason=stop" in r.stdout, r.stdout[-300:])


def main():
    check("agent-core 存在", os.path.exists(CORE), CORE)
    fake, port = start_fake()
    base = "http://127.0.0.1:%d" % port
    try:
        test_compaction_event(base)
        test_compacted_request_shape(base, port)
        test_budget_zero_disables(base)
        test_cli_usage_unknown(base)
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
