#!/usr/bin/env python3
"""S8 验收：权限、审批与沙箱（策略闸门）。

用法:
    python3 tests/test_s8.py [agent-core] [agent-cli] [agent-run]

覆盖：
  1. workdir 内的写操作直接 allow（不打扰用户）
  2. workdir 外的写：ask -> allow，工具真的执行
  3. workdir 外的写：ask -> deny，工具不执行
  4. 危险命令 ask -> deny；审批结果进会话日志
  5. --approval=never：危险命令直接 deny，不发 approval.requested
  6. --approval=allow：危险命令直接 allow
  7. approval 超时 = deny，turn 正常结算
  8. approval.respond 的错误 id / 非法 decision / ack
  9. cli 行内 y/n（allow 与 deny 两条路径）
 10. agent-run 无人值守不挂起（默认 never，ask 时自动 deny）
 11. 路径穿越 ../ 也按 workdir 外处理
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / "build"
FAKE = HERE / "fake_llm_tools.py"

positional = [a for a in sys.argv[1:] if not a.startswith("--")]
CORE = positional[0] if len(positional) > 0 else str(BUILD / "agent-core")
CLI = positional[1] if len(positional) > 1 else str(BUILD / "agent-cli")
RUN = positional[2] if len(positional) > 2 else str(BUILD / "agent-run")

OUTSIDE = "/tmp/s8-outside.txt"
ESCAPE = "/tmp/s8-escape.txt"
TARGET = "/tmp/s8-target"

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


def core_env(home):
    env = dict(os.environ)
    env.pop("OPENAI_API_KEY", None)
    env["DEEPSEEK_API_KEY"] = "sk-s8-test"
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
    return hello, recv(p)


def approval_response(p, rid, approval_id, decision):
    send(p, {"type": "request", "id": rid, "method": "approval.respond",
             "params": {"approval_id": approval_id, "decision": decision}})
    return recv(p)


def chat_with_approval(p, rid, text, decision=None, timeout=15):
    """跑一轮；遇到 approval.requested 时按 decision 回（None = 不回，等超时）。"""
    send(p, {"type": "request", "id": rid, "method": "chat", "params": {"text": text}})
    events = []
    approval = None
    acks = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        f = recv(p)
        if f.get("type") == "event":
            events.append(f)
            if f.get("name") == "approval.requested":
                approval = f
                if decision is not None:
                    payload = f.get("payload", {})
                    send(p, {"type": "request", "id": rid + "-a", "method": "approval.respond",
                             "params": {"approval_id": payload.get("approval_id", ""),
                                        "decision": decision}})
            continue
        if f.get("type") == "response" and f.get("id") != rid:
            acks.append(f)
            continue
        return events, f, approval, acks
    raise TimeoutError("chat_with_approval timed out")


def event_names(events):
    return [e.get("name") for e in events]


def tool_result(events):
    rs = [e for e in events if e.get("name") == "tool.result"]
    return rs[-1].get("payload", {}) if rs else {}


def approval_event(events):
    for e in events:
        if e.get("name") == "approval.requested":
            return e.get("payload", {})
    return None


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


def rm_file(path):
    try:
        os.remove(path)
    except FileNotFoundError:
        pass


def rm_dir(path):
    shutil.rmtree(path, ignore_errors=True)


def make_target():
    rm_dir(TARGET)
    os.makedirs(TARGET)
    with open(os.path.join(TARGET, "keep.txt"), "w", encoding="utf-8") as f:
        f.write("keep\n")


def target_alive():
    return os.path.isdir(TARGET) and os.path.exists(os.path.join(TARGET, "keep.txt"))


def workdir():
    return tempfile.mkdtemp(prefix="s8-wd-")


# ---------------------------------------------------------------- 1. 安全操作


def test_safe_within_workdir(base):
    home = tempfile.mkdtemp(prefix="s8-safe-")
    wd = workdir()
    p = spawn_core(home, "--tools", "all", "--workdir", wd,
                   "--base-url", base + "/write", "--model", "fake",
                   "--approval", "ask", "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        events, resp, approval, _ = chat_with_approval(p, "r1", "写 out.txt")
        check("workdir 内写正常结算", resp.get("ok") is True, str(resp))
        check("workdir 内写不触发审批", approval is None, str(event_names(events)))
        tr = tool_result(events)
        check("workdir 内写工具成功", tr.get("ok") is True, str(tr))
        check("workdir 内文件真的写了", os.path.exists(os.path.join(wd, "out.txt")))
    finally:
        shutdown(p)


# ---------------------------------------------------------------- 2/3. 越界写


def test_outside_write_allow(base):
    home = tempfile.mkdtemp(prefix="s8-out-allow-")
    wd = workdir()
    rm_file(OUTSIDE)
    p = spawn_core(home, "--tools", "all", "--workdir", wd,
                   "--base-url", base + "/writeabs", "--model", "fake",
                   "--approval", "ask", "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        events, resp, approval, acks = chat_with_approval(p, "r1", "写外面", decision="allow")
        check("越界写 ask 发出 approval.requested", approval is not None, str(event_names(events)))
        ap = approval_event(events) or {}
        check("approval 带 name/arguments/reason", ap.get("name") == "write_file" and
              "path" in (ap.get("arguments") or {}) and ap.get("reason"), str(ap))
        check("allow 后工具成功", tool_result(events).get("ok") is True, str(tool_result(events)))
        check("allow 后文件真的写了", os.path.exists(OUTSIDE), OUTSIDE)
        check("ack 是 ok", acks and acks[-1].get("ok") is True, str(acks[-1:]))
        check("本轮正常结算", resp.get("ok") is True, str(resp))
    finally:
        shutdown(p)
        rm_file(OUTSIDE)


def test_outside_write_deny(base):
    home = tempfile.mkdtemp(prefix="s8-out-deny-")
    wd = workdir()
    rm_file(OUTSIDE)
    p = spawn_core(home, "--tools", "all", "--workdir", wd,
                   "--base-url", base + "/writeabs", "--model", "fake",
                   "--approval", "ask", "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        events, resp, approval, _ = chat_with_approval(p, "r1", "写外面", decision="deny")
        check("deny 时确实问了", approval is not None)
        tr = tool_result(events)
        check("deny 后工具失败", tr.get("ok") is False, str(tr))
        check("deny 错误可行动", "approval denied" in tr.get("error", ""), str(tr))
        check("deny 后文件不存在", not os.path.exists(OUTSIDE), OUTSIDE)
        check("deny 不影响本轮结算", resp.get("ok") is True, str(resp))
    finally:
        shutdown(p)
        rm_file(OUTSIDE)


# ---------------------------------------------------------------- 4/5/6. 危险命令


def test_dangerous_rm_deny_and_session_log(base):
    home = tempfile.mkdtemp(prefix="s8-rm-")
    sdir = os.path.join(home, "sessions")
    wd = workdir()
    make_target()
    p = spawn_core(home, "--tools", "all", "--workdir", wd,
                   "--base-url", base + "/rmrf", "--model", "fake",
                   "--approval", "ask", "--session", "s1", "--session-dir", sdir,
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        events, resp, approval, _ = chat_with_approval(p, "r1", "删掉目标", decision="deny")
        check("rm -rf 触发审批", approval is not None, str(event_names(events)))
        ap = approval_event(events) or {}
        check("审批原因提到 rm/delete", "rm" in ap.get("reason", "").lower() or "delete" in ap.get("reason", "").lower(),
              ap.get("reason", ""))
        tr = tool_result(events)
        check("rm -rf 被拒绝", tr.get("ok") is False and "approval denied" in tr.get("error", ""), str(tr))
        check("rm -rf 真的没执行", target_alive(), TARGET)
        check("deny 后本轮正常结算", resp.get("ok") is True, str(resp))
    finally:
        shutdown(p)

    rows = read_jsonl(os.path.join(sdir, "s1.jsonl"))
    approvals = [r for r in rows if r.get("type") == "event" and r.get("name") == "approval.requested"]
    check("会话日志里有 approval.requested", len(approvals) >= 1, str(len(approvals)))
    commits = [r for r in rows if r.get("type") == "session.commit"]
    msgs = commits[-1].get("messages", []) if commits else []
    tool_msgs = [m for m in msgs if m.get("role") == "tool"]
    check("会话日志 commit 里有审批错误", any("approval denied" in (m.get("content") or "") for m in tool_msgs),
          str(tool_msgs[-1:]))
    rm_dir(TARGET)


def test_approval_never(base):
    home = tempfile.mkdtemp(prefix="s8-never-")
    wd = workdir()
    make_target()
    p = spawn_core(home, "--tools", "all", "--workdir", wd,
                   "--base-url", base + "/rmrf", "--model", "fake",
                   "--approval", "never", "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        events, resp, approval, _ = chat_with_approval(p, "r1", "删掉目标")
        check("never 模式不发 approval.requested", approval is None, str(event_names(events)))
        tr = tool_result(events)
        check("never 模式直接 deny", tr.get("ok") is False and "policy=never" in tr.get("error", ""), str(tr))
        check("never 模式工具没执行", target_alive(), TARGET)
        check("never 模式本轮正常结算", resp.get("ok") is True, str(resp))
    finally:
        shutdown(p)
        rm_dir(TARGET)


def test_approval_allow(base):
    home = tempfile.mkdtemp(prefix="s8-allow-")
    wd = workdir()
    p = spawn_core(home, "--tools", "all", "--workdir", wd,
                   "--base-url", base + "/bashc", "--model", "fake",
                   "--approval", "allow", "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        events, resp, approval, _ = chat_with_approval(p, "r1", "跑 bash")
        check("allow 模式不发 approval.requested", approval is None, str(event_names(events)))
        tr = tool_result(events)
        check("allow 模式危险命令执行了", tr.get("ok") is True and "hi" in tr.get("output", ""), str(tr))
        check("allow 模式本轮正常结算", resp.get("ok") is True, str(resp))
    finally:
        shutdown(p)


# ---------------------------------------------------------------- 7/8. 超时与协议错误


def test_approval_timeout(base):
    home = tempfile.mkdtemp(prefix="s8-timeout-")
    wd = workdir()
    make_target()
    p = spawn_core(home, "--tools", "all", "--workdir", wd,
                   "--base-url", base + "/rmrf", "--model", "fake",
                   "--approval", "ask", "--approval-timeout-ms", "250",
                   "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        events, resp, approval, _ = chat_with_approval(p, "r1", "删掉目标", decision=None)
        check("超时前发了 approval.requested", approval is not None)
        tr = tool_result(events)
        check("超时 = deny", tr.get("ok") is False and "timed out" in tr.get("error", ""), str(tr))
        check("超时后工具没执行", target_alive(), TARGET)
        check("超时后本轮正常结算", resp.get("ok") is True, str(resp))
    finally:
        shutdown(p)
        rm_dir(TARGET)


def test_approval_protocol_errors(base):
    home = tempfile.mkdtemp(prefix="s8-proto-")
    wd = workdir()
    make_target()
    p = spawn_core(home, "--tools", "all", "--workdir", wd,
                   "--base-url", base + "/rmrf", "--model", "fake",
                   "--approval", "ask", "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        send(p, {"type": "request", "id": "r1", "method": "chat", "params": {"text": "删掉目标"}})
        events = []
        ap = None
        while ap is None:
            f = recv(p)
            if f.get("type") == "event":
                events.append(f)
                if f.get("name") == "approval.requested":
                    ap = f.get("payload", {})
        aid = ap.get("approval_id", "")

        r = approval_response(p, "x1", "bogus", "allow")
        check("错误 approval_id -> NO_PENDING_APPROVAL",
              r.get("ok") is False and r.get("error", {}).get("code") == "NO_PENDING_APPROVAL", str(r))
        r = approval_response(p, "x2", aid, "maybe")
        check("非法 decision -> INVALID_PARAMS",
              r.get("ok") is False and r.get("error", {}).get("code") == "INVALID_PARAMS", str(r))
        r = approval_response(p, "x3", aid, "deny")
        check("正确 deny -> ack ok", r.get("ok") is True and r.get("result", {}).get("decision") == "deny", str(r))

        while True:
            f = recv(p)
            if f.get("type") == "event":
                events.append(f)
                continue
            if f.get("type") == "response" and f.get("id") == "r1":
                check("错误尝试后仍能正常结算", f.get("ok") is True, str(f))
                break
        check("最终工具被拒绝", tool_result(events).get("ok") is False, str(tool_result(events)))
    finally:
        shutdown(p)
        rm_dir(TARGET)


# ---------------------------------------------------------------- 9. cli y/n


def test_cli_y_n(base):
    home = tempfile.mkdtemp(prefix="s8-cli-")
    wd = workdir()
    env = core_env(home)
    rm_file(OUTSIDE)

    r = subprocess.run([CLI, "--tools", "all", "--workdir", wd,
                        "--base-url", base + "/writeabs", "--model", "fake",
                        "--approval", "ask", "--delta-batch-ms=0", "--max-context-tokens=0"],
                       input="写外面\ny\n", capture_output=True, text=True, timeout=40, env=env)
    check("cli allow 退出 0", r.returncode == 0, r.stdout[-400:] + r.stderr[-400:])
    check("cli 打出 approval 提示", "[approval]" in r.stdout, r.stdout[-400:])
    check("cli allow 后文件存在", os.path.exists(OUTSIDE), OUTSIDE)

    rm_file(OUTSIDE)
    r = subprocess.run([CLI, "--tools", "all", "--workdir", wd,
                        "--base-url", base + "/writeabs", "--model", "fake",
                        "--approval", "ask", "--delta-batch-ms=0", "--max-context-tokens=0"],
                       input="写外面\nn\n", capture_output=True, text=True, timeout=40, env=env)
    check("cli deny 退出 0", r.returncode == 0, r.stdout[-400:] + r.stderr[-400:])
    check("cli deny 后文件不存在", not os.path.exists(OUTSIDE), OUTSIDE)
    rm_file(OUTSIDE)


# ---------------------------------------------------------------- 10. agent-run


def test_agent_run_unattended(base):
    home = tempfile.mkdtemp(prefix="s8-run-")
    wd = workdir()
    make_target()
    env = core_env(home)

    r = subprocess.run([RUN, "--core", CORE,
                        "--base-url", base + "/rmrf", "--model", "fake",
                        "--tools", "all", "--workdir", wd, "删掉目标"],
                       capture_output=True, text=True, timeout=25, env=env, stdin=subprocess.DEVNULL)
    check("agent-run 默认 never 不挂起", r.returncode == 0, r.stdout[-400:] + r.stderr[-400:])
    check("agent-run 默认 never 拒绝危险命令", target_alive(), TARGET)
    check("agent-run stderr 有工具失败", "[tool]" in r.stderr and "error" in r.stderr, r.stderr[-400:])

    r = subprocess.run([RUN, "--core", CORE,
                        "--base-url", base + "/rmrf", "--model", "fake",
                        "--tools", "all", "--workdir", wd, "--approval", "ask", "删掉目标"],
                       capture_output=True, text=True, timeout=25, env=env, stdin=subprocess.DEVNULL)
    check("agent-run --approval=ask 也不挂起", r.returncode == 0, r.stdout[-400:] + r.stderr[-400:])
    check("agent-run 自动 deny 留有 stderr 记录", "[approval]" in r.stderr, r.stderr[-400:])
    check("agent-run 自动 deny 后目标还在", target_alive(), TARGET)
    rm_dir(TARGET)


# ---------------------------------------------------------------- 11. 路径穿越


def test_path_traversal(base):
    home = tempfile.mkdtemp(prefix="s8-trav-")
    wd = workdir()
    rm_file(ESCAPE)
    p = spawn_core(home, "--tools", "all", "--workdir", wd,
                   "--base-url", base + "/writetraversal", "--model", "fake",
                   "--approval", "ask", "--delta-batch-ms=0", "--max-context-tokens=0")
    try:
        handshake(p)
        events, resp, approval, _ = chat_with_approval(p, "r1", "穿越", decision="deny")
        check("../ 被识别为 workdir 外", approval is not None, str(event_names(events)))
        check("../ 被拒绝", tool_result(events).get("ok") is False, str(tool_result(events)))
        check("../ 没有写出文件", not os.path.exists(ESCAPE), ESCAPE)
        check("路径穿越轮正常结算", resp.get("ok") is True, str(resp))
    finally:
        shutdown(p)
        rm_file(ESCAPE)


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
    fake, port = start_fake()
    base = "http://127.0.0.1:%d" % port
    try:
        run_case("1 workdir 内直接 allow", test_safe_within_workdir, base)
        run_case("2 越界写 ask -> allow", test_outside_write_allow, base)
        run_case("3 越界写 ask -> deny", test_outside_write_deny, base)
        run_case("4 危险命令 deny + 会话日志", test_dangerous_rm_deny_and_session_log, base)
        run_case("5 --approval=never", test_approval_never, base)
        run_case("6 --approval=allow", test_approval_allow, base)
        run_case("7 approval 超时", test_approval_timeout, base)
        run_case("8 approval 协议错误", test_approval_protocol_errors, base)
        run_case("9 cli y/n", test_cli_y_n, base)
        run_case("10 agent-run 无人值守", test_agent_run_unattended, base)
        run_case("11 路径穿越", test_path_traversal, base)
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