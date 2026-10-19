#!/usr/bin/env python3
"""S3a 验收：真 Provider（非流式）。

覆盖：成功回复 / 多轮历史 / 错误映射（401·500·非 JSON·连接失败·超时）/,
      失败轮次不回灌历史 / cli 端到端 / --demo 仍可用 / 分层铁律。

用法: python3 tests/test_s3a.py [agent-core] [agent-cli]
"""
import json
import os
import subprocess
import sys
import time
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


def start_fake():
    p = subprocess.Popen([sys.executable, str(HERE / "fake_llm.py")],
                         stdout=subprocess.PIPE, text=True)
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
    return json.loads(line)


def handshake(p):
    recv(p)  # event hello
    send(p, {"type": "request", "id": "h1", "method": "hello",
             "params": {"protocol_version": "1.0", "client_version": "test"}})
    return recv(p)


def chat(p, rid, text):
    send(p, {"type": "request", "id": rid, "method": "chat", "params": {"text": text}})
    # S3b 起 chat 会先吐 assistant.delta 事件（protocol.md §9 S3b 明写了这件事）。
    # 本阶段只关心"最后结算的那一帧"，所以跳过事件帧——下面的断言一个字都没改。
    while True:
        f = recv(p)
        if f.get("type") == "response":
            return f


def kill(p):
    try:
        p.stdin.close()
        p.wait(timeout=5)
    except Exception:
        p.kill()


def test_success_and_history(base):
    p = spawn_core("--base-url", base, "--model", "fake")
    handshake(p)
    r = chat(p, "r1", "你好")
    check("chat 成功并回整段文本", r["ok"] and r["result"]["text"] == "fake(1 msgs) echo=你好", str(r))
    check("chat 带 stop_reason", r["result"].get("stop_reason") == "stop")
    check("chat 带回 usage", r["result"].get("usage", {}).get("prompt_tokens") == 1,
          str(r["result"].get("usage")))

    r2 = chat(p, "r2", "第二句")
    check("多轮：历史在 core（第 2 轮发了 3 条消息）",
          r2["ok"] and r2["result"]["text"] == "fake(3 msgs) echo=第二句", str(r2))
    kill(p)


def test_error_mapping(base):
    p = spawn_core("--base-url", base + "/error401", "--model", "fake")
    handshake(p)
    r = chat(p, "r1", "会失败")
    check("401 -> PROVIDER_ERROR 且带状态码",
          not r["ok"] and r["error"]["code"] == "PROVIDER_ERROR" and "401" in r["error"]["message"],
          str(r))
    kill(p)

    p = spawn_core("--base-url", base + "/error500", "--model", "fake")
    handshake(p)
    r = chat(p, "r1", "会失败")
    check("500 -> PROVIDER_ERROR",
          not r["ok"] and r["error"]["code"] == "PROVIDER_ERROR" and "500" in r["error"]["message"],
          str(r))
    kill(p)

    p = spawn_core("--base-url", base + "/garbage", "--model", "fake")
    handshake(p)
    r = chat(p, "r1", "会失败")
    check("非 JSON 正文 -> PROVIDER_ERROR",
          not r["ok"] and r["error"]["code"] == "PROVIDER_ERROR" and "not JSON" in r["error"]["message"],
          str(r))
    kill(p)


def test_transport_failures(base):
    t0 = time.time()
    p = spawn_core("--base-url", "http://127.0.0.1:1", "--model", "fake",
                   "--provider-timeout-ms=3000")
    handshake(p)
    r = chat(p, "r1", "连不上")
    dt = time.time() - t0
    check("连接失败 -> PROVIDER_ERROR（快失败，不挂死）",
          not r["ok"] and r["error"]["code"] == "PROVIDER_ERROR" and dt < 8,
          "%.2fs %s" % (dt, r))
    kill(p)

    t0 = time.time()
    p = spawn_core("--base-url", base + "/slow", "--model", "fake",
                   "--provider-timeout-ms=600")
    handshake(p)
    r = chat(p, "r1", "超时")
    dt = time.time() - t0
    check("读超时 -> PROVIDER_ERROR（不等满 3 秒）",
          not r["ok"] and r["error"]["code"] == "PROVIDER_ERROR" and dt < 2.5,
          "%.2fs %s" % (dt, r))
    kill(p)


def test_failed_turn_not_in_history(base):
    p = spawn_core("--base-url", base + "/failonce", "--model", "fake")
    handshake(p)
    r = chat(p, "r1", "第一次会失败")
    check("failonce：首轮失败", not r["ok"] and r["error"]["code"] == "PROVIDER_ERROR", str(r))
    r = chat(p, "r2", "第二次")
    check("失败的一轮没有回灌历史（第 2 次只发 1 条消息）",
          r["ok"] and r["result"]["text"] == "fake(1 msgs) echo=第二次", str(r))
    kill(p)


def test_no_provider():
    p = spawn_core(env=core_env(with_key=False))
    handshake(p)
    r = chat(p, "r1", "没人可问")
    check("无 provider -> NO_PROVIDER（可行动的报错）",
          not r["ok"] and r["error"]["code"] == "NO_PROVIDER" and "--base-url" in r["error"]["message"],
          str(r))
    kill(p)


def test_api_key_flag(base):
    env = core_env(with_key=False)
    p = spawn_core("--base-url", base, "--model", "fake", "--api-key", "sk-from-flag", env=env)
    handshake(p)
    r = chat(p, "r1", "用参数给 key")
    check("--api-key 参数可用（环境变量缺失时）", r["ok"], str(r))
    kill(p)


def test_ambiguous_config_warnings(base):
    env = core_env()
    env["OPENAI_API_KEY"] = "sk-another"
    r = subprocess.run([CORE, "--base-url", base, "--model", "fake"], input="",
                       capture_output=True, text=True, timeout=10, env=env)
    check("两个 key 同时存在 -> 明确告警（不静默选一个）",
          "both DEEPSEEK_API_KEY and OPENAI_API_KEY" in r.stderr, r.stderr)

    r = subprocess.run([CORE, "--base-url", "http://127.0.0.1:1"], input="",
                       capture_output=True, text=True, timeout=10, env=env)
    check("非 deepseek 端点且未给 --model -> 明确告警",
          "defaulting to deepseek-chat" in r.stderr, r.stderr)


def test_cli(base, env):
    r = subprocess.run([CLI, "--base-url", base, "--model", "fake"],
                       input="你好\n第二句\n", capture_output=True, text=True, timeout=40, env=env)
    out = r.stdout
    check("cli: 真模型路径可用",
          "[assistant] fake(1 msgs) echo=你好" in out and "[assistant] fake(3 msgs) echo=第二句" in out, out)
    check("cli: 结算行带 stop_reason 与 tokens",
          "[turn t1 done: stop_reason=stop, tokens=" in out, out)

    r = subprocess.run([CLI, "--demo", "--demo-delay-ms=0"], input="你好\n",
                       capture_output=True, text=True, timeout=40, env=env)
    check("cli --demo 仍走 S2 假模型",
          "[assistant] 收到第 1 句" in r.stdout and "deltas, stop_reason=stop" in r.stdout, r.stdout)

    r = subprocess.run([CLI], input="你好\n", capture_output=True, text=True, timeout=40,
                       env=core_env(with_key=False))
    check("cli 无 key 时给出可行动的报错", "[error] NO_PROVIDER" in r.stdout, r.stdout + r.stderr)


def test_layering():
    bad = []
    for path in (HERE.parent / "src" / "cli").rglob("*.cpp"):
        text = path.read_text(encoding="utf-8")
        for needle in ("httplib", "openssl", "ftxui"):
            if needle in text:
                bad.append("%s: %s" % (path.name, needle))
    check("铁律：cli 源码不含 httplib / openssl / ftxui", not bad, str(bad))


def main():
    fake, port = start_fake()
    base = "http://127.0.0.1:%d" % port
    env = core_env()
    try:
        test_success_and_history(base)
        test_error_mapping(base)
        test_transport_failures(base)
        test_failed_turn_not_in_history(base)
        test_no_provider()
        test_ambiguous_config_warnings(base)
        test_api_key_flag(base)
        test_cli(base, env)
        test_layering()
    finally:
        fake.kill()


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
    print("ALL S3A TESTS PASSED")
