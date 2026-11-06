#!/usr/bin/env python3
"""S9 验收：FTXUI TUI（pty 端到端）。

用法:
    python3 tests/test_s9.py [agent-core] [agent-tui]

覆盖：
  * demo 模式：TUI 启动、输入、流式回答、Ctrl-C 退出
  * 工具调用在 TUI 里可见（read_file -> 答案）
  * 审批弹窗：写 workdir 外 -> y/n；n 后不写文件
  * TuiModel 的纯状态由 tests/tui_model_test.cpp 单独覆盖
"""
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / "build"
FAKE = HERE / "fake_llm_tools.py"

positional = [a for a in sys.argv[1:] if not a.startswith("--")]
CORE = positional[0] if len(positional) > 0 else str(BUILD / "agent-core")
TUI = positional[1] if len(positional) > 1 else str(BUILD / "agent-tui")

OUTSIDE = "/tmp/s9-outside.txt"
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


def env():
    e = dict(os.environ)
    e.pop("OPENAI_API_KEY", None)
    e["DEEPSEEK_API_KEY"] = "sk-s9-test"
    e["TERM"] = "xterm-256color"
    e["LANG"] = "C.UTF-8"
    e["LC_ALL"] = "C.UTF-8"
    return e


class PtyProc:
    def __init__(self, argv):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execve(argv[0], argv, env())
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 110, 0, 0))
        self.buf = b""

    def send(self, text):
        os.write(self.fd, text.encode("utf-8"))

    def read_until(self, needles, timeout=12):
        deadline = time.time() + timeout
        while time.time() < deadline:
            text = self.buf.decode("utf-8", "replace")
            for n in needles:
                if n in text:
                    return True, text
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 8192)
            except OSError:
                break
            if not chunk:
                break
            self.buf += chunk
        return False, self.buf.decode("utf-8", "replace")

    def close(self, timeout=6):
        try:
            self.send("\x03")
        except OSError:
            pass
        deadline = time.time() + timeout
        status = None
        while time.time() < deadline:
            wpid, st = os.waitpid(self.pid, os.WNOHANG)
            if wpid == self.pid:
                status = st
                break
            time.sleep(0.05)
        if status is None:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            _, status = os.waitpid(self.pid, 0)
        try:
            os.close(self.fd)
        except OSError:
            pass
        return status


def run_demo():
    p = PtyProc([TUI, "--core", CORE, "--demo", "--demo-delay-ms=0"])
    try:
        ok, text = p.read_until(["yxAgent TUI"], 10)
        check("TUI 启动并渲染标题", ok, text[-500:])
        p.send("你好\r")
        ok, text = p.read_until(["收到第 1 句", "收到第"], 10)
        check("TUI 输入后收到流式回答", ok, text[-800:])
        check("回答里带用户输入", "你好" in text, text[-500:])
    finally:
        status = p.close()
        check("Ctrl-C 让 TUI 干净退出", status is not None and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
              str(status))


def make_wd():
    wd = tempfile.mkdtemp(prefix="s9-wd-")
    with open(os.path.join(wd, "a.txt"), "w", encoding="utf-8") as f:
        f.write("第一行\n第二行\n第三行\n")
    return wd


def run_tool(base):
    wd = make_wd()
    p = PtyProc([TUI, "--core", CORE, "--base-url", base + "/tools", "--model", "fake",
                 "--tools", "all", "--workdir", wd, "--approval", "ask"])
    try:
        ok, text = p.read_until(["yxAgent TUI"], 10)
        check("工具测试 TUI 启动", ok, text[-500:])
        p.send("统计 a.txt 的行数\r")
        ok, text = p.read_until(["read_file"], 10)
        check("TUI 里能看到 tool.call", ok, text[-1000:])
        ok, text = p.read_until(["a.txt 共 3 行", "共 3 行", "3 行"], 12)
        check("TUI 里能看到最终答案", ok, text[-1000:])
    finally:
        status = p.close()
        check("工具测试退出 0", status is not None and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
              str(status))


def run_approval(base):
    wd = make_wd()
    try:
        os.remove(OUTSIDE)
    except FileNotFoundError:
        pass
    p = PtyProc([TUI, "--core", CORE, "--base-url", base + "/writeabs", "--model", "fake",
                 "--tools", "all", "--workdir", wd, "--approval", "ask"])
    try:
        ok, text = p.read_until(["yxAgent TUI"], 10)
        check("审批测试 TUI 启动", ok, text[-500:])
        p.send("写外面\r")
        ok, text = p.read_until(["审批请求", "[approval]"], 12)
        check("越界写弹出审批", ok, text[-1200:])
        check("审批里带 write_file", "write_file" in text, text[-800:])
        p.send("n")
        ok, text = p.read_until(["denied", "deny"], 10)
        check("TUI 显示 deny", ok, text[-1000:])
        check("deny 后文件不存在", not os.path.exists(OUTSIDE), OUTSIDE)
    finally:
        status = p.close()
        check("审批测试退出 0", status is not None and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
              str(status))
    try:
        os.remove(OUTSIDE)
    except FileNotFoundError:
        pass


def test_layering():
    src = (HERE.parent / "src/tui/main.cpp").read_text(encoding="utf-8")
    check("agent-tui 不 include core 内部头", '#include "core/' not in src, "found core include")
    check("agent-tui 不碰 httplib/openssl", "httplib" not in src and "openssl" not in src,
          "found network include")
    cmake = (HERE.parent / "CMakeLists.txt").read_text(encoding="utf-8")
    check("agent-tui 只链 agent_common + FTXUI",
          "target_link_libraries(agent-tui PRIVATE agent_common component dom screen Threads::Threads)" in cmake,
          "CMake target missing")
    check("FTXUI 是 vendored 的",
          (HERE.parent / "third_party/ftxui/LICENSE").exists(), "third_party/ftxui missing")


def main():
    check("agent-core 存在", os.path.exists(CORE), CORE)
    check("agent-tui 存在", os.path.exists(TUI), TUI)
    fake, port = start_fake()
    base = "http://127.0.0.1:%d" % port
    try:
        run_demo()
        run_tool(base)
        run_approval(base)
        test_layering()
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