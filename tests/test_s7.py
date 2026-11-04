#!/usr/bin/env python3
"""S7 验收：会话持久化、resume 与 headless 前端。

用法:
    python3 tests/test_s7.py [agent-core] [agent-cli] [agent-run]

覆盖 10 组：
  1. 落盘格式（NDJSON、header、commit、seq 唯一、0600）
  2. 关掉再开，history_ 还在（真实发给模型的 messages 里能看到）
  3. seq 跨连接续号 + turn 号继续
  4. S6 的 [压缩] 摘要在重启后仍在，且不套娃
  5. 工具调用历史（assistant(tool_calls) + tool）原样恢复
  6. 失败轮不落盘（session.abort，不污染下一次请求）
  7. 轮中 SIGKILL：未提交轮被忽略，seq 不复用
  8. session.load 的 create / load / resume / 错误码
  9. cli --session / --resume 端到端
 10. agent-run headless + 只靠协议的分层检查
"""
import json
import os
import re
import signal
import stat
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / "build"
FAKE_SSE = HERE / "fake_llm_sse.py"
FAKE_TOOLS = HERE / "fake_llm_tools.py"

positional = [a for a in sys.argv[1:] if not a.startswith("--")]
CORE = positional[0] if len(positional) > 0 else str(BUILD / "agent-core")
CLI = positional[1] if len(positional) > 1 else str(BUILD / "agent-cli")
RUN = positional[2] if len(positional) > 2 else str(BUILD / "agent-run")

fails = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (("   " + detail) if (detail and not cond) else ""))
    if not cond:
        fails.append(name)


def start_fake(script):
    p = subprocess.Popen([sys.executable, str(script)], stdout=subprocess.PIPE, text=True)
    line = p.stdout.readline().strip()
    if not line.startswith("PORT="):
        raise RuntimeError("fake server did not print PORT=, got %r" % line)
    return p, int(line.split("=")[1])


def core_env(home):
    env = dict(os.environ)
    env.pop("OPENAI_API_KEY", None)
    env["DEEPSEEK_API_KEY"] = "sk-s7-test"
    env["HOME"] = home
    return env


def spawn_core(home, *extra):
    return subprocess.Popen([CORE, *extra], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            text=True, env=core_env(home))


def send(p, obj):
    p.stdin.write(json.dumps(obj, ensure_ascii=False) + "\n")
    p.stdin.flush()


def recv(p):
    line = p.stdout.readline()
    if not line:
        raise RuntimeError("core closed stdout unexpectedly")
    return json.loads(line)


def handshake(p):
    hello = recv(p)
    if hello.get("type") != "event" or hello.get("name") != "hello":
        raise RuntimeError("first frame is not hello: %r" % hello)
    send(p, {"type": "request", "id": "h1", "method": "hello",
             "params": {"protocol_version": "1.0", "client_version": "test"}})
    welcome = recv(p)
    return hello, welcome


def chat(p, rid, text):
    send(p, {"type": "request", "id": rid, "method": "chat", "params": {"text": text}})
    events = []
    while True:
        f = recv(p)
        if f.get("type") == "event":
            events.append(f)
            continue
        return events, f


def session_load(p, rid, params):
    send(p, {"type": "request", "id": rid, "method": "session.load", "params": params})
    while True:
        f = recv(p)
        if f.get("type") == "response" and f.get("id") == rid:
            return f


def shutdown(p):
    try:
        send(p, {"type": "request", "id": "bye", "method": "shutdown", "params": {}})
        while True:
            f = recv(p)
            if f.get("type") == "response" and f.get("id") == "bye":
                break
    except Exception:
        pass
    try:
        p.stdin.close()
    except Exception:
        pass
    try:
        p.wait(timeout=5)
    except Exception:
        p.kill()


def last_request(port):
    with urllib.request.urlopen("http://127.0.0.1:%d/__last" % port, timeout=5) as r:
        return json.load(r)


def read_jsonl(path):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def session_path(home, sid="s1"):
    return os.path.join(home, "sessions", sid + ".jsonl")


def chat_texts(info):
    return [m.get("content", "") for m in (info.get("messages_full") or [])]


def roles(info):
    return [m.get("role") for m in (info.get("messages_full") or [])]


def has_tool_call(info, call_id):
    for m in info.get("messages_full") or []:
        for tc in m.get("tool_calls") or []:
            if tc.get("id") == call_id:
                return True
    return False


# ---------------------------------------------------------------- 1. 格式


def test_file_format(base):
    home = tempfile.mkdtemp(prefix="s7-fmt-")
    p = spawn_core(home, "--session", "s1", "--session-dir", os.path.join(home, "sessions"),
                   "--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        hello, welcome = handshake(p)
        check("新建会话时 hello 带 session", hello.get("payload", {}).get("session", {}).get("id") == "s1")
        check("新建会话 resumed=false", hello["payload"]["session"].get("resumed") is False)
        events, resp = chat(p, "r1", "第一句")
        check("chat 正常结算", resp.get("ok") is True, str(resp))
    finally:
        shutdown(p)

    path = session_path(home)
    check("会话文件存在", os.path.exists(path), path)
    check("会话文件权限 0600", stat.S_IMODE(os.stat(path).st_mode) == 0o600,
          oct(stat.S_IMODE(os.stat(path).st_mode)))
    rows = read_jsonl(path)
    check("第一行是 session.header", rows and rows[0].get("type") == "session.header", str(rows[:1]))
    check("header 版本 v1", rows[0].get("v") == 1 and rows[0].get("id") == "s1", str(rows[0]))
    types = [r.get("type") for r in rows]
    check("有初始 checkpoint", "session.checkpoint" in types, str(types))
    check("有 turn.begin", "session.turn.begin" in types, str(types))
    check("有 commit（一轮一行）", "session.commit" in types, str(types))
    commits = [r for r in rows if r.get("type") == "session.commit"]
    check("commit 带 messages", commits and isinstance(commits[-1].get("messages"), list), str(commits[-1:]))
    msgs = commits[-1]["messages"] if commits else []
    check("commit 里有 user", any(m.get("role") == "user" for m in msgs), str(msgs))
    check("commit 里有 assistant", any(m.get("role") == "assistant" for m in msgs), str(msgs))
    seqs = [r["seq"] for r in rows if r.get("type") == "event"]
    check("event 帧有 seq 且严格递增", seqs == sorted(seqs) and len(seqs) == len(set(seqs)), str(seqs))
    check("有 request/response 审计帧", "request" in types and "response" in types, str(types))


# ---------------------------------------------------------------- 2. 历史恢复


def test_resume_history(base, port):
    home = tempfile.mkdtemp(prefix="s7-hist-")
    sdir = os.path.join(home, "sessions")
    first = "第一句：我叫小蓝"
    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0",
                   "--system", "你是测试助手")
    try:
        handshake(p)
        _, resp = chat(p, "r1", first)
        check("首轮正常", resp.get("ok") is True, str(resp))
    finally:
        shutdown(p)

    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0",
                   "--system", "你是测试助手")
    try:
        hello, _ = handshake(p)
        check("resume 后 hello 标 resumed", hello["payload"]["session"].get("resumed") is True)
        _, resp = chat(p, "r2", "第二句：我叫什么")
        check("resume 后能接着问", resp.get("ok") is True, str(resp))
        info = last_request(port)
        rs = roles(info)
        texts = chat_texts(info)
        check("resume 后请求仍有 system 在第 0 条", rs and rs[0] == "system", str(rs))
        check("resume 后请求里有第一句", any(first in t for t in texts), str(texts))
        check("resume 后请求里有第二轮 user", any("第二句" in t for t in texts), str(texts))
        check("旧 user 只出现一次", sum(1 for t in texts if first in t) == 1, str(texts))
        check("assistant 历史也在", any(r == "assistant" and t for r, t in zip(rs, texts)), str(rs))
    finally:
        shutdown(p)


# ---------------------------------------------------------------- 3. seq / turn


def test_seq_and_turn(base):
    home = tempfile.mkdtemp(prefix="s7-seq-")
    sdir = os.path.join(home, "sessions")
    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    seqs1 = []
    try:
        handshake(p)
        for i in range(2):
            ev, resp = chat(p, "r%d" % i, "第%d句" % (i + 1))
            check("第 %d 轮正常" % (i + 1), resp.get("ok") is True, str(resp))
            seqs1 += [e["seq"] for e in ev]
            check("turn_id 递增", resp["result"].get("turn_id") == "t%d" % (i + 1), str(resp["result"]))
    finally:
        shutdown(p)
    max1 = max(seqs1) if seqs1 else 0

    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        hello, _ = handshake(p)
        hello_seq = hello.get("seq", 0)
        check("resume 后 hello seq = 上次最大 seq + 1", hello_seq == max1 + 1, "%s vs %s" % (hello_seq, max1 + 1))
        ev, resp = chat(p, "r9", "第三句")
        check("resume 后 turn 号继续", resp["result"].get("turn_id") == "t3", str(resp["result"]))
        allseq = [hello_seq] + [e["seq"] for e in ev]
        check("resume 后 seq 仍严格递增", allseq == sorted(allseq) and len(allseq) == len(set(allseq)), str(allseq))
        check("resume 后 seq 全部大于旧最大", all(s > max1 for s in allseq), str(allseq))
    finally:
        shutdown(p)

    rows = read_jsonl(session_path(home))
    seqs = [r["seq"] for r in rows if r.get("type") == "event"]
    check("会话文件里的 seq 无重号", len(seqs) == len(set(seqs)), str(seqs))
    check("会话文件里的 seq 严格递增", seqs == sorted(seqs), str(seqs))


# ---------------------------------------------------------------- 4. 压缩摘要恢复


def test_compaction_resume(base, port):
    home = tempfile.mkdtemp(prefix="s7-comp-")
    sdir = os.path.join(home, "sessions")
    fact = "早期事实：代号是蓝鲸"
    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=60", "--keep-recent-turns=1",
                   "--system", "你是测试助手")
    saw_compaction = False
    try:
        handshake(p)
        _, resp = chat(p, "r0", fact)
        check("压缩测试首轮正常", resp.get("ok") is True, str(resp))
        for i in range(1, 8):
            ev, resp = chat(p, "r%d" % i, "第%d句：" % i + "y" * 300)
            if any(e.get("name") == "context.compacted" for e in ev):
                saw_compaction = True
            check("压缩测试第 %d 轮正常" % (i + 1), resp.get("ok") is True, str(resp))
    finally:
        shutdown(p)
    check("触发了 context.compacted", saw_compaction)

    rows = read_jsonl(session_path(home))
    checkpoints = [r for r in rows if r.get("type") == "session.commit" and "checkpoint" in r]
    check("压缩轮写了 session.commit.checkpoint", len(checkpoints) >= 1, str(len(checkpoints)))
    if checkpoints:
        msgs = checkpoints[-1]["checkpoint"]
        check("checkpoint 的 system 仍在第 0 条", msgs and msgs[0].get("role") == "system", str(msgs[:1]))
        summaries = [m for m in msgs if m.get("role") == "user" and "[压缩]" in (m.get("content") or "")]
        check("checkpoint 里有 [压缩] 摘要", len(summaries) == 1, str(len(summaries)))
        check("摘要里带着早期事实", summaries and fact in summaries[0].get("content", ""),
              summaries[0].get("content", "")[:160] if summaries else "no summary")

    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=60", "--keep-recent-turns=1",
                   "--system", "你是测试助手")
    try:
        handshake(p)
        _, resp = chat(p, "r99", "继续")
        check("压缩后 resume 正常", resp.get("ok") is True, str(resp))
        info = last_request(port)
        rs = roles(info)
        texts = chat_texts(info)
        check("resume 后 system 仍只有一条", rs.count("system") == 1, str(rs))
        summaries = [t for t in texts if "[压缩]" in t]
        check("resume 后摘要只有一条（不套娃）", len(summaries) == 1, str(len(summaries)))
    finally:
        shutdown(p)


# ---------------------------------------------------------------- 5. 工具历史


def test_tool_history(base_tools, port_tools):
    home = tempfile.mkdtemp(prefix="s7-tool-")
    sdir = os.path.join(home, "sessions")
    wd = tempfile.mkdtemp(prefix="s7-wd-")
    with open(os.path.join(wd, "a.txt"), "w", encoding="utf-8") as f:
        f.write("第一行\n第二行\n第三行\n")

    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base_tools + "/tools", "--model", "fake",
                   "--tools", "all", "--workdir", wd,
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        ev, resp = chat(p, "r1", "统计 a.txt 的行数")
        check("工具轮正常", resp.get("ok") is True, str(resp))
        check("发出 tool.call", any(e.get("name") == "tool.call" for e in ev), str([e.get("name") for e in ev]))
    finally:
        shutdown(p)

    rows = read_jsonl(session_path(home))
    commits = [r for r in rows if r.get("type") == "session.commit"]
    msgs = commits[-1]["messages"] if commits else []
    assistants = [m for m in msgs if m.get("role") == "assistant" and m.get("tool_calls")]
    tools = [m for m in msgs if m.get("role") == "tool"]
    check("commit 里有 assistant(tool_calls)", len(assistants) >= 1, str(msgs))
    check("commit 里有 tool 结果", len(tools) >= 1, str(msgs))
    if assistants and tools:
        call_id = assistants[0]["tool_calls"][0].get("id")
        check("tool_call_id 与 call_id 配对", any(t.get("tool_call_id") == call_id for t in tools), str(tools))
        check("工具名保留", assistants[0]["tool_calls"][0].get("name") == "read_file")

    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base_tools + "/plain", "--model", "fake",
                   "--tools", "all", "--workdir", wd,
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        _, resp = chat(p, "r2", "继续")
        check("工具历史 resume 后正常", resp.get("ok") is True, str(resp))
        info = last_request(port_tools)
        check("请求里带 assistant tool_calls", info.get("has_tool_calls_in_history") is True, str(info))
        # 没有孤儿 tool：每个 tool 都必须能找到先前 assistant 的 call_id
        ok = True
        seen = set()
        for m in info.get("messages_full") or []:
            for tc in m.get("tool_calls") or []:
                seen.add(tc.get("id"))
            if m.get("role") == "tool" and m.get("tool_call_id") not in seen:
                ok = False
        check("没有孤儿 tool 消息", ok, str(roles(info)))
    finally:
        shutdown(p)


# ---------------------------------------------------------------- 6. 失败轮


def test_failed_turn(base, port):
    home = tempfile.mkdtemp(prefix="s7-fail-")
    sdir = os.path.join(home, "sessions")
    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base + "/error500", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        _, resp = chat(p, "r1", "这轮会失败")
        check("失败轮返回 error", resp.get("ok") is False, str(resp))
    finally:
        shutdown(p)

    rows = read_jsonl(session_path(home))
    check("失败轮写了 session.abort", any(r.get("type") == "session.abort" for r in rows), str([r.get("type") for r in rows]))
    check("失败轮没有 commit", not any(r.get("type") == "session.commit" and r.get("turn") == 1 for r in rows))

    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        _, resp = chat(p, "r2", "失败之后的新问题")
        check("失败后 resume 正常", resp.get("ok") is True, str(resp))
        info = last_request(port)
        texts = chat_texts(info)
        check("失败轮的 user 不在历史里", not any("这轮会失败" in t for t in texts), str(texts))
        check("新问题在历史里", any("失败之后的新问题" in t for t in texts), str(texts))
    finally:
        shutdown(p)


# ---------------------------------------------------------------- 7. 轮中崩溃


def test_crash_mid_turn(base, port):
    home = tempfile.mkdtemp(prefix="s7-crash-")
    sdir = os.path.join(home, "sessions")
    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base + "/manyslow", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    crashed_prompt = "这条会被 SIGKILL 打断"
    try:
        handshake(p)
        send(p, {"type": "request", "id": "r1", "method": "chat", "params": {"text": crashed_prompt}})
        saw_delta = False
        while True:
            f = recv(p)
            if f.get("type") == "event" and f.get("name") == "assistant.delta":
                saw_delta = True
                break
        check("崩溃前收到了 delta", saw_delta)
        p.kill()
        p.wait(timeout=5)
    except Exception as e:
        check("崩溃测试准备失败", False, str(e))
        try:
            p.kill()
        except Exception:
            pass

    rows = read_jsonl(session_path(home))
    check("崩溃后保留 turn.begin", any(r.get("type") == "session.turn.begin" and r.get("turn") == 1 for r in rows),
          str([r.get("type") for r in rows]))
    check("崩溃的轮没有 commit", not any(r.get("type") == "session.commit" and r.get("turn") == 1 for r in rows))
    max_seq = max([r["seq"] for r in rows if r.get("type") == "event"] or [0])

    p = spawn_core(home, "--session", "s1", "--session-dir", sdir,
                   "--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        hello, _ = handshake(p)
        check("崩溃后 seq 不复用", hello.get("seq", 0) == max_seq + 1, "%s vs %s" % (hello.get("seq", 0), max_seq + 1))
        _, resp = chat(p, "r2", "恢复后的新问题")
        check("崩溃后 resume 正常", resp.get("ok") is True, str(resp))
        info = last_request(port)
        texts = chat_texts(info)
        check("崩溃轮的 user 不在历史里", not any(crashed_prompt in t for t in texts), str(texts))
        check("崩溃后新问题在历史里", any("恢复后的新问题" in t for t in texts), str(texts))
    finally:
        shutdown(p)


# ---------------------------------------------------------------- 8. session.load


def test_session_load(base, port):
    home = tempfile.mkdtemp(prefix="s7-load-")
    sdir = os.path.join(home, "sessions")
    p = spawn_core(home, "--session-dir", sdir,
                   "--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0",
                   "--system", "你是测试助手")
    try:
        hello, _ = handshake(p)
        check("无会话启动时 hello 没有 session", "session" not in hello.get("payload", {}))
        r = session_load(p, "l1", {"resume": True})
        check("空目录 resume -> SESSION_NOT_FOUND", r.get("ok") is False and r["error"]["code"] == "SESSION_NOT_FOUND", str(r))
        r = session_load(p, "l2", {"id": "../x", "create": True})
        check("非法 id -> INVALID_PARAMS", r.get("ok") is False and r["error"]["code"] == "INVALID_PARAMS", str(r))
        r = session_load(p, "l3", {"id": "missing"})
        check("不存在的 id（无 create）-> SESSION_NOT_FOUND", r.get("ok") is False and r["error"]["code"] == "SESSION_NOT_FOUND", str(r))
        r = session_load(p, "l4", {"id": "s1", "create": True})
        check("session.load create 成功", r.get("ok") is True and r["result"].get("created") is True, str(r))
        check("create 后 turns=0", r["result"].get("turns") == 0, str(r["result"]))
        _, resp = chat(p, "r1", "加载后的第一句")
        check("加载后 chat 正常", resp.get("ok") is True, str(resp))
        r = session_load(p, "l5", {"id": "s1"})
        check("重复 load 幂等", r.get("ok") is True and r["result"].get("created") is False, str(r))
        check("load 返回 last_seq", r["result"].get("last_seq", 0) > 0, str(r["result"]))
        r = session_load(p, "l5b", {"resume": True})
        check("已挂会话 resume 幂等", r.get("ok") is True and r["result"].get("id") == "s1", str(r))

        # 切到一个全新会话：不能把 s1 的 history_ 克隆过去
        r = session_load(p, "l6", {"id": "s2", "create": True})
        check("切到新会话成功", r.get("ok") is True and r["result"].get("id") == "s2", str(r))
        _, resp = chat(p, "r2", "第二会话")
        check("新会话 chat 正常", resp.get("ok") is True, str(resp))
        texts = chat_texts(last_request(port))
        check("新会话不克隆旧历史", not any("加载后的第一句" in t for t in texts), str(texts))
        check("新会话只有 system + 新 user", texts[:1] == ["你是测试助手"] and "第二会话" in texts, str(texts))

        # 再切回 s1：旧历史还在
        r = session_load(p, "l7", {"id": "s1"})
        check("切回 s1 成功", r.get("ok") is True and r["result"].get("id") == "s1", str(r))
        _, resp = chat(p, "r3", "回到一")
        check("切回后 chat 正常", resp.get("ok") is True, str(resp))
        texts = chat_texts(last_request(port))
        check("切回后旧历史还在", any("加载后的第一句" in t for t in texts), str(texts))

        # 同一个会话第二个写者：flock 必须拒绝
        second = subprocess.run([CORE, "--session", "s1", "--session-dir", sdir,
                                 "--base-url", base + "/many", "--model", "fake"],
                                input="", capture_output=True, text=True, timeout=15, env=core_env(home))
        check("第二个写者 SESSION_BUSY", second.returncode == 2 and "SESSION_BUSY" in second.stderr,
              "rc=%s stderr=%s" % (second.returncode, second.stderr[-200:]))
    finally:
        shutdown(p)

    p = spawn_core(home, "--session-dir", sdir,
                   "--base-url", base + "/many", "--model", "fake",
                   "--delta-batch-ms=0", "--max-context-tokens=0",
                   "--system", "你是测试助手")
    try:
        handshake(p)
        r = session_load(p, "l8", {"id": "s1"})
        check("新进程 load 已存在会话", r.get("ok") is True and r["result"].get("turns") >= 1, str(r))
    finally:
        shutdown(p)


# ---------------------------------------------------------------- 9. cli


def test_cli_resume(base, port):
    home = tempfile.mkdtemp(prefix="s7-cli-")
    sdir = os.path.join(home, "sessions")
    env = core_env(home)
    r = subprocess.run([CLI, "--session", "s1", "--session-dir", sdir,
                        "--base-url", base + "/many", "--model", "fake",
                        "--delta-batch-ms=0", "--max-context-tokens=0"],
                       input="第一句\n", capture_output=True, text=True, timeout=60, env=env)
    check("cli 首跑退出 0", r.returncode == 0, r.stdout[-400:] + r.stderr[-400:])
    check("cli 首跑显示 new s1", "[session] new s1" in r.stdout, r.stdout[-400:])

    r = subprocess.run([CLI, "--resume", "--session-dir", sdir,
                        "--base-url", base + "/many", "--model", "fake",
                        "--delta-batch-ms=0", "--max-context-tokens=0"],
                       input="第二句\n", capture_output=True, text=True, timeout=60, env=env)
    check("cli --resume 退出 0", r.returncode == 0, r.stdout[-400:] + r.stderr[-400:])
    check("cli --resume 显示 resumed s1", "[session] resumed s1" in r.stdout, r.stdout[-400:])
    info = last_request(port)
    texts = chat_texts(info)
    check("cli resume 后模型看到第一句", any("第一句" in t for t in texts), str(texts))


# ---------------------------------------------------------------- 10. agent-run


def test_agent_run(base_tools, port_tools):
    home = tempfile.mkdtemp(prefix="s7-run-")
    sdir = os.path.join(home, "sessions")
    wd = tempfile.mkdtemp(prefix="s7-runwd-")
    with open(os.path.join(wd, "a.txt"), "w", encoding="utf-8") as f:
        f.write("第一行\n第二行\n第三行\n")
    env = core_env(home)

    r = subprocess.run([RUN, "--core", CORE,
                        "--base-url", base_tools + "/tools", "--model", "fake",
                        "--tools", "all", "--workdir", wd,
                        "--session", "s1", "--session-dir", sdir,
                        "统计 a.txt 的行数"],
                       capture_output=True, text=True, timeout=60, env=env, stdin=subprocess.DEVNULL)
    check("agent-run 退出 0", r.returncode == 0, r.stdout[-400:] + r.stderr[-400:])
    check("agent-run stdout 有最终答案", "3 行" in r.stdout, r.stdout[-400:])
    check("agent-run 工具进度走 stderr", "[tool]" in r.stderr, r.stderr[-400:])
    check("agent-run stdout 不混工具行", "[tool]" not in r.stdout, r.stdout[-400:])

    r = subprocess.run([RUN, "--core", CORE,
                        "--base-url", base_tools + "/plain", "--model", "fake",
                        "--tools", "all", "--workdir", wd,
                        "--resume=s1", "--session-dir", sdir,
                        "继续"],
                       capture_output=True, text=True, timeout=60, env=env, stdin=subprocess.DEVNULL)
    check("agent-run --resume 退出 0", r.returncode == 0, r.stdout[-400:] + r.stderr[-400:])
    info = last_request(port_tools)
    texts = chat_texts(info)
    check("agent-run resume 后模型看到上一轮答案", any("3 行" in t for t in texts), str(texts))

    src = (HERE.parent / "src/run/main.cpp").read_text(encoding="utf-8")
    check("agent-run 不 include core 内部头", '#include "core/' not in src, "found core include")
    check("agent-run 不碰 httplib/openssl", "httplib" not in src and "openssl" not in src, "found network include")
    cmake = (HERE.parent / "CMakeLists.txt").read_text(encoding="utf-8")
    check("agent-run 只链 agent_common",
          re.search(r"add_executable\(agent-run src/run/main\.cpp\)", cmake) is not None and
          re.search(r"target_link_libraries\(agent-run PRIVATE agent_common\)", cmake) is not None,
          "CMake target missing")


# ---------------------------------------------------------------- main


def run_case(name, fn, *args):
    print("\n--- " + name + " ---")
    try:
        fn(*args)
    except Exception as e:
        check(name + " 未抛异常", False, repr(e))


def main():
    check("agent-core 存在", os.path.exists(CORE), CORE)
    check("agent-cli 存在", os.path.exists(CLI), CLI)
    check("agent-run 存在", os.path.exists(RUN), RUN)
    fake_sse, port_sse = start_fake(FAKE_SSE)
    fake_tools, port_tools = start_fake(FAKE_TOOLS)
    base = "http://127.0.0.1:%d" % port_sse
    base_tools = "http://127.0.0.1:%d" % port_tools
    try:
        run_case("1 落盘格式", test_file_format, base)
        run_case("2 历史恢复", test_resume_history, base, port_sse)
        run_case("3 seq 与 turn", test_seq_and_turn, base)
        run_case("4 压缩摘要恢复", test_compaction_resume, base, port_sse)
        run_case("5 工具历史恢复", test_tool_history, base_tools, port_tools)
        run_case("6 失败轮", test_failed_turn, base, port_sse)
        run_case("7 轮中崩溃", test_crash_mid_turn, base, port_sse)
        run_case("8 session.load", test_session_load, base, port_sse)
        run_case("9 cli resume", test_cli_resume, base, port_sse)
        run_case("10 agent-run", test_agent_run, base_tools, port_tools)
    finally:
        fake_sse.terminate()
        fake_tools.terminate()
    print()
    if fails:
        print("FAILED: %d" % len(fails))
        for n in fails:
            print("  - " + n)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())