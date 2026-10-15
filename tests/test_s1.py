#!/usr/bin/env python3
"""S1 验收测试：直接 spawn agent-core，对照 docs/protocol.md 逐条验证。"""
import json
import subprocess
import sys
from pathlib import Path

# 默认按"本脚本所在位置"定位二进制：<repo>/build/agent-core —— 从任何目录运行都成立。
# 也可以显式指定：python3 tests/test_s1.py [path/to/agent-core]
_here = Path(__file__).resolve().parent
core = sys.argv[1] if len(sys.argv) > 1 else str(_here.parent / "build" / "agent-core")
if not Path(core).exists():
    print(f"agent-core not found: {core}")
    print("先构建:  cmake --build build    或显式指定: python3 tests/test_s1.py <path/to/agent-core>")
    sys.exit(2)
print(f"testing: {core}")
fails = []

def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (("   " + detail) if (detail and not cond) else ""))
    if not cond:
        fails.append(name)

def spawn():
    return subprocess.Popen([core], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

def send(p, obj):
    p.stdin.write(json.dumps(obj) + "\n")
    p.stdin.flush()

# ---- 生命周期 1：正常会话 ----
p = spawn()
first = json.loads(p.stdout.readline())
check("first frame is event hello seq=1", first["type"] == "event" and first["name"] == "hello" and first["seq"] == 1, str(first))
check("hello payload carries versions", first["payload"]["protocol_version"] == "1.0" and "core_version" in first["payload"])

send(p, {"type": "request", "id": "r1", "method": "hello", "params": {"protocol_version": "1.0", "client_version": "test"}})
r = json.loads(p.stdout.readline())
check("hello ok (welcome semantics)", r["type"] == "response" and r["id"] == "r1" and r["ok"] is True and r["result"]["protocol_version"] == "1.0", str(r))

send(p, {"type": "request", "id": "r2", "method": "ping", "params": {"client_ts": 123}})
r = json.loads(p.stdout.readline())
check("ping echoes client_ts, adds core_ts", r["ok"] and r["result"]["client_ts"] == 123 and isinstance(r["result"]["core_ts"], int), str(r))

send(p, {"type": "request", "id": "r3", "method": "echo", "params": {"line": "hi"}})
r = json.loads(p.stdout.readline())
check("echo roundtrip", r["ok"] and r["result"] == {"line": "hi"}, str(r))

send(p, {"type": "request", "id": "r4", "method": "nope", "params": {}})
r = json.loads(p.stdout.readline())
check("unknown method -> UNKNOWN_METHOD", not r["ok"] and r["error"]["code"] == "UNKNOWN_METHOD", str(r))

p.stdin.write("this is not json\n")
p.stdin.flush()
r = json.loads(p.stdout.readline())
check("garbage -> PARSE_ERROR with id null", not r["ok"] and r["error"]["code"] == "PARSE_ERROR" and r["id"] is None, str(r))

send(p, {"type": "event", "seq": 99, "name": "bogus", "payload": {}})
send(p, {"type": "request", "id": "r5", "method": "echo", "params": {"line": "still alive"}})
r = json.loads(p.stdout.readline())
check("unknown event ignored (no reply)", r["ok"] and r["id"] == "r5" and r["result"]["line"] == "still alive", str(r))

send(p, {"type": "request", "id": "r6", "method": "shutdown", "params": {}})
r = json.loads(p.stdout.readline())
code = p.wait(timeout=5)
check("shutdown acknowledged", r["ok"] and r["result"]["will_exit"] is True, str(r))
check("clean exit code 0", code == 0, str(code))

# ---- 生命周期 2：版本不兼容 -> 退出码 2 ----
p2 = spawn()
json.loads(p2.stdout.readline())
send(p2, {"type": "request", "id": "r1", "method": "hello", "params": {"protocol_version": "2.0"}})
r = json.loads(p2.stdout.readline())
code2 = p2.wait(timeout=5)
check("major mismatch -> PROTOCOL_VERSION_MISMATCH", not r["ok"] and r["error"]["code"] == "PROTOCOL_VERSION_MISMATCH", str(r))
check("mismatch exit code 2", code2 == 2, str(code2))

print()
if fails:
    print("FAILED:", len(fails))
    sys.exit(1)
print("ALL S1 TESTS PASSED")
