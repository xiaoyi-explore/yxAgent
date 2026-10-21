#!/usr/bin/env python3
"""S4 验收：Agent 循环与工具调用（ReAct）。

覆盖：read_file/write_file/run_shell 真执行、arguments 分片累积（S4 头号坑）、
      多工具、工具失败回灌、坏 JSON 参数、命令超时看门狗、MAX_STEPS、
      非流式路径、未知工具、tools 声明的开与关、cli 端到端、事件顺序不变量。

用法: python3 tests/test_s4.py [agent-core] [agent-cli]
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

positional = [a for a in sys.argv[1:] if not a.startswith("--")]
CORE = positional[0] if len(positional) > 0 else str(BUILD / "agent-core")
CLI = positional[1] if len(positional) > 1 else str(BUILD / "agent-cli")

FILE_BODY = "第一行" + chr(10) + "第二行" + chr(10) + "第三行" + chr(10)  # 3 行

fails = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (("   " + detail) if (detail and not cond) else ""))
    if not cond:
        fails.append(name)


def start_fake():
    p = subprocess.Popen([sys.executable, str(HERE / "fake_llm_tools.py")],
                         stdout=subprocess.PIPE, text=True)
    line = p.stdout.readline().strip()
    if not line.startswith("PORT="):
        raise RuntimeError("fake server did not report a port: " + line)
    return p, int(line.split("=")[1])


def core_env():
    env = dict(os.environ)
    env.pop("OPENAI_API_KEY", None)
    env["DEEPSEEK_API_KEY"] = "sk-test"
    return env


def spawn_core(*extra):
    return subprocess.Popen([CORE, *extra], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            text=True, env=core_env())


def send(p, obj):
    p.stdin.write(json.dumps(obj, ensure_ascii=False) + chr(10))
    p.stdin.flush()


def recv(p):
    line = p.stdout.readline()
    if not line:
        raise RuntimeError("core closed stdout unexpectedly")
    return json.loads(line)


def handshake(p):
    recv(p)
    send(p, {"type": "request", "id": "h1", "method": "hello",
             "params": {"protocol_version": "1.0", "client_version": "test"}})
    return recv(p)


def turn(p, rid, text):
    """跑一个 turn，收集事件与响应。返回 dict。"""
    send(p, {"type": "request", "id": rid, "method": "chat", "params": {"text": text}})
    out = {"deltas": [], "steps": [], "calls": [], "results": [], "seqs": [], "resp": None}
    while True:
        f = recv(p)
        if f.get("type") == "event":
            out["seqs"].append(f.get("seq"))
            name = f.get("name", "")
            payload = f.get("payload", {})
            if name == "assistant.delta":
                out["deltas"].append(payload.get("text", ""))
                out["steps"].append(payload.get("step", 1))
            elif name == "tool.call":
                out["calls"].append(payload)
            elif name == "tool.result":
                out["results"].append(payload)
            continue
        out["resp"] = f
        return out


def kill(p):
    try:
        p.stdin.close()
        p.wait(timeout=5)
    except Exception:
        p.kill()


def final_text(r):
    resp = r["resp"]
    return resp.get("result", {}).get("text", "") if resp.get("ok") else ""


def workdir():
    d = tempfile.mkdtemp(prefix="s4-")
    with open(os.path.join(d, "a.txt"), "w", encoding="utf-8") as f:
        f.write(FILE_BODY)
    return d


def seq_ok(seqs):
    return all(b == a + 1 for a, b in zip(seqs, seqs[1:]))


# ---------------------------------------------------------------- 正常路径
def test_basic(base, wd):
    p = spawn_core("--base-url", base + "/tools", "--model", "fake", "--tools", "all", "--workdir", wd)
    handshake(p)
    r = turn(p, "r1", "统计 a.txt 有多少行")
    check("read_file：发出了 tool.call 且参数是结构化对象",
          len(r["calls"]) == 1 and r["calls"][0]["name"] == "read_file"
          and r["calls"][0]["arguments"] == {"path": "a.txt"}, str(r["calls"])[:300])
    check("read_file：tool.result ok 且内容是文件原文",
          len(r["results"]) == 1 and r["results"][0]["ok"] and r["results"][0]["output"] == FILE_BODY,
          str(r["results"])[:300])
    check("turn 两步收尾（模型 -> 工具 -> 模型）", r["resp"]["ok"] and r["resp"]["result"]["steps"] == 2,
          str(r["resp"])[:300])
    check("响应里的 tool_calls 计数是 1", r["resp"]["result"].get("tool_calls") == 1, str(r["resp"])[:200])
    check("最终答复是模型根据工具输出写的", final_text(r) == "a.txt 共 3 行", repr(final_text(r)))
    check("事件 seq 严格递增无空洞", seq_ok(r["seqs"]), str(r["seqs"]))
    last_step = r["resp"]["result"]["steps"]
    check("不变量 2：result.text == 最后一个 step 的增量拼接",
          "".join(d for d, s in zip(r["deltas"], r["steps"]) if s == last_step) == final_text(r),
          repr(r["deltas"]))
    check("tool.call 一定在它的 tool.result 之前",
          r["calls"] and r["results"] and r["calls"][0]["call_id"] == r["results"][0]["call_id"])
    kill(p)


def test_arguments_split(base, wd):
    """S4 的头号坑：arguments 分片到达，必须按 index 累积拼接。"""
    p = spawn_core("--base-url", base + "/toolsplit", "--model", "fake", "--tools", "all", "--workdir", wd)
    handshake(p)
    r = turn(p, "r1", "统计 a.txt")
    check("分片参数被拼成正确的 JSON（切在中间也不怕）",
          len(r["calls"]) == 1 and r["calls"][0]["arguments"] == {"path": "a.txt"}, str(r["calls"])[:300])
    check("分片工具名被拼全（name 也分两片到达）",
          r["calls"] and r["calls"][0]["name"] == "read_file", str(r["calls"])[:200])
    check("分片参数：工具确实被执行了", len(r["results"]) == 1 and r["results"][0]["ok"]
          and r["results"][0]["output"] == FILE_BODY, str(r["results"])[:200])
    kill(p)


def test_multitool(base, wd):
    p = spawn_core("--base-url", base + "/multitool", "--model", "fake", "--tools", "all", "--workdir", wd)
    handshake(p)
    r = turn(p, "r1", "两个工具一起来")
    check("一轮里两个工具都被调用（index 0/1 交错）", len(r["calls"]) == 2, str(r["calls"])[:300])
    check("两个工具的 call_id 不重复",
          len({c["call_id"] for c in r["calls"]}) == 2, str([c["call_id"] for c in r["calls"]]))
    check("两个工具都执行成功", all(x["ok"] for x in r["results"]), str(r["results"])[:300])
    check("run_shell 的输出进了事件", any("hi" in (x.get("output") or "") or "second" in (x.get("output") or "")
                                          for x in r["results"]), str([x.get("output") for x in r["results"]])[:300])
    check("响应 tool_calls == 2", r["resp"]["result"].get("tool_calls") == 2, str(r["resp"])[:200])
    kill(p)


def test_tool_error_fed_back(base, wd):
    p = spawn_core("--base-url", base + "/toolerr", "--model", "fake", "--tools", "all", "--workdir", wd)
    handshake(p)
    r = turn(p, "r1", "读一个不存在的文件")
    check("工具失败：ok=false 且 error 可读",
          len(r["results"]) == 1 and not r["results"][0]["ok"] and "cannot open" in r["results"][0]["error"],
          str(r["results"])[:300])
    check("失败的 ERROR 文本被回灌给模型（答复里能看到）",
          "ERROR" in final_text(r), repr(final_text(r)))
    check("工具失败不算整个 turn 失败（模型有机会自我纠正）", r["resp"]["ok"], str(r["resp"])[:200])
    kill(p)


def test_bad_arguments(base, wd):
    p = spawn_core("--base-url", base + "/badargs", "--model", "fake", "--tools", "all", "--workdir", wd)
    handshake(p)
    r = turn(p, "r1", "参数写坏了")
    check("坏 JSON 参数：tool.call 里带原文字段 arguments_raw",
          len(r["calls"]) == 1 and "arguments_raw" in r["calls"][0], str(r["calls"])[:300])
    check("坏 JSON 参数：不执行工具，ok=false 且说明原因",
          len(r["results"]) == 1 and not r["results"][0]["ok"]
          and "not a valid JSON object" in r["results"][0]["error"], str(r["results"])[:300])
    check("坏 JSON 参数：错误也回灌给模型", "参数被拒绝了" in final_text(r), repr(final_text(r)))
    kill(p)


# ---------------------------------------------------------------- 两个内置工具
def test_run_shell(base, wd):
    p = spawn_core("--base-url", base + "/shell", "--model", "fake", "--tools", "all", "--workdir", wd)
    handshake(p)
    r = turn(p, "r1", "跑个命令")
    out = r["results"][0].get("output", "") if r["results"] else ""
    check("run_shell：输出里有 exit_code=0 与命令结果",
          r["results"] and r["results"][0]["ok"] and "exit_code=0" in out and "hi from shell" in out, repr(out))
    kill(p)


def test_shell_timeout(base, wd):
    p = spawn_core("--base-url", base + "/sleeptool", "--model", "fake", "--tools", "all", "--workdir", wd)
    handshake(p)
    t0 = time.time()
    r = turn(p, "r1", "跑个会超时的命令")
    dt = time.time() - t0
    check("run_shell：超时被看门狗杀掉（不要挂死 turn）",
          r["results"] and not r["results"][0]["ok"] and "timeout" in r["results"][0]["error"],
          str(r["results"])[:300])
    check("run_shell：确实在 300ms 左右就返回了（没有被 sleep 5 拖住）", dt < 4, "%.2fs" % dt)
    check("run_shell：超时耗时记录在 duration_ms", r["results"] and r["results"][0]["duration_ms"] >= 250,
          str(r["results"][0].get("duration_ms") if r["results"] else None))
    kill(p)


def test_write_file(base, wd):
    p = spawn_core("--base-url", base + "/write", "--model", "fake", "--tools", "all", "--workdir", wd)
    handshake(p)
    r = turn(p, "r1", "写个文件")
    target = os.path.join(wd, "out.txt")
    check("write_file：文件真的写到了 workdir 下",
          os.path.exists(target) and open(target, encoding="utf-8").read() == "written by tool" + chr(10),
          repr(open(target, encoding="utf-8").read()) if os.path.exists(target) else "missing")
    check("write_file：事件里报告了写入字节数", r["results"] and "wrote" in r["results"][0]["output"],
          str(r["results"])[:200])
    kill(p)


# ---------------------------------------------------------------- 循环边界
def test_max_steps(base, wd):
    p = spawn_core("--base-url", base + "/loop", "--model", "fake", "--tools", "all",
                   "--workdir", wd, "--max-steps", "3")
    handshake(p)
    r = turn(p, "r1", "永远不收尾")
    check("MAX_STEPS：模型不肯收尾时被硬上限拦下",
          not r["resp"]["ok"] and r["resp"]["error"]["code"] == "MAX_STEPS", str(r["resp"])[:300])
    check("MAX_STEPS：正好跑了 3 次工具（= --max-steps 3）", len(r["calls"]) == 3, str(len(r["calls"])))

    send(p, {"type": "request", "id": "p1", "method": "ping", "params": {}})
    check("MAX_STEPS 之后 core 还活着（ping 仍能应答）", recv(p).get("ok") is True)
    kill(p)


def test_unknown_tool(base, wd):
    """模型点名一个没注册的工具：不能崩，要作为工具错误回灌。"""
    p = spawn_core("--base-url", base + "/shell", "--model", "fake", "--tools", "read_file", "--workdir", wd)
    handshake(p)
    r = turn(p, "r1", "跑个命令（但我只给了 read_file）")
    check("未注册的工具：ok=false 且写明 unknown tool",
          r["results"] and not r["results"][0]["ok"] and "unknown tool" in r["results"][0]["error"],
          str(r["results"])[:300])
    kill(p)


# ---------------------------------------------------------------- 非流式 & 声明
def test_nonstream_tools(base, wd):
    p = spawn_core("--no-stream", "--base-url", base + "/tools", "--model", "fake",
                   "--tools", "all", "--workdir", wd)
    handshake(p)
    r = turn(p, "r1", "非流式也要能用工具")
    check("非流式路径：tool_calls 也能从整段 JSON 里解出来",
          len(r["calls"]) == 1 and r["calls"][0]["arguments"] == {"path": "a.txt"}, str(r["calls"])[:300])
    check("非流式路径：同样是两步收尾", r["resp"]["ok"] and r["resp"]["result"]["steps"] == 2, str(r["resp"])[:200])
    kill(p)


def last_request(base):
    with urllib.request.urlopen(base + "/__last", timeout=5) as resp:
        return json.loads(resp.read().decode("utf-8"))


def test_tools_declared(base, wd):
    p = spawn_core("--base-url", base + "/tools", "--model", "fake", "--tools", "all", "--workdir", wd)
    handshake(p)
    turn(p, "r1", "看看请求")
    kill(p)
    last = last_request(base)
    check("--tools all：请求里声明了三个工具",
          last.get("tools") == ["read_file", "write_file", "run_shell"], str(last.get("tools")))
    check("工具声明带 JSON Schema（type=object + required）",
          all((s or {}).get("type") == "object" and (s or {}).get("required") for s in last.get("tool_schemas", [])),
          str(last.get("tool_schemas"))[:300])
    check("工具结果确实回灌进了下一轮请求（历史里有 tool 角色且带着 tool_calls）",
          last.get("has_tool_calls_in_history") is True and "tool" in (last.get("roles") or []),
          str(last.get("roles")))

    # 不给 --tools：请求里就不该有 tools 字段（S3b 的行为）
    p = spawn_core("--base-url", base + "/tools", "--model", "fake", "--workdir", wd)
    handshake(p)
    turn(p, "r1", "没有工具")
    kill(p)
    last = last_request(base)
    check("不给 --tools：请求里没有 tools 声明（默认关闭）", last.get("tools") == [], str(last.get("tools")))


# ---------------------------------------------------------------- cli 端到端
def test_cli(base, wd):
    r = subprocess.run([CLI, "--base-url", base + "/tools", "--model", "fake",
                        "--tools", "all", "--workdir", wd],
                       input="统计 a.txt 有多少行" + chr(10), capture_output=True, text=True,
                       timeout=60, env=core_env())
    out = r.stdout
    check("cli：把 tool.call 画出来了", "[tool] read_file" in out, out)
    check("cli：把 tool.result 的 ok 画出来了", "-> ok" in out, out)
    check("cli：最终答复是统计结果", "a.txt 共 3 行" in out, out)
    check("cli：结算行带上 steps/tools", "steps=2, tools=1" in out, out)
    check("cli：没有把整段文字重播一遍（[assistant] 与答案各只出现一次）",
          out.count("[assistant]") == 1 and out.count("a.txt 共 3 行") == 1, out)


def test_layering():
    checks = []
    tools_src = (HERE.parent / "src" / "core" / "tools.cpp").read_text(encoding="utf-8")
    for needle in ("httplib", "openssl"):
        if needle in tools_src:
            checks.append("tools.cpp: " + needle)
    check("铁律：工具层不认识 HTTP（tools.cpp 里没有 httplib/openssl）", not checks, str(checks))

    cli_src = (HERE.parent / "src" / "cli" / "main.cpp").read_text(encoding="utf-8")
    check("铁律：前端不执行工具（cli 源码里没有 read_file/run_shell 的实现痕迹）",
          "popen" not in cli_src and "ToolRegistry" not in cli_src, cli_src[:200])


def main():
    fake, port = start_fake()
    base = "http://127.0.0.1:%d" % port
    wd = workdir()
    try:
        test_basic(base, wd)
        test_arguments_split(base, wd)
        test_multitool(base, wd)
        test_tool_error_fed_back(base, wd)
        test_bad_arguments(base, wd)
        test_run_shell(base, wd)
        test_shell_timeout(base, wd)
        test_write_file(base, wd)
        test_max_steps(base, wd)
        test_unknown_tool(base, wd)
        test_nonstream_tools(base, wd)
        test_tools_declared(base, wd)
        test_cli(base, wd)
        test_layering()
    finally:
        fake.kill()
        shutil.rmtree(wd, ignore_errors=True)


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
    print("ALL S4 TESTS PASSED")
