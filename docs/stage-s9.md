# S9 —— TUI 诞生（tag `s9`）⭐ 里程碑 M4

> **一句话**：出现 `agent-tui`（FTXUI）：输入框 + 消息列表 + 流式打字机 + 状态栏 + 审批弹窗；
> cli 保留作调试前端。它消费的是**真实**的 delta / tool.call / tool.result / approval.requested。
>
> **环境说明**：构建机离线，拿不到系统 FTXUI，也装不了包。所以把 **FTXUI v5.0.0** vendored 进
> `third_party/ftxui`（MIT，见 LICENSE / VENDORED.md）；只保留 `include/src/cmake`，
> 砍掉 examples/doc，并注释掉 FTXUI CMakeLists 尾部对它们的 add_subdirectory。

---

## 1. 交付物

| 交付物 | 落点 |
|---|---|
| `agent-tui`（FTXUI 全屏 UI） | `src/tui/main.cpp` |
| core 子进程 + 阻塞读帧 | `src/tui/core_client.hpp/.cpp` |
| 纯状态 TuiModel（不依赖 FTXUI） | `src/tui/tui_model.hpp/.cpp` |
| FTXUI v5.0.0（vendored） | `third_party/ftxui/` |
| 模型单元测试 | `tests/tui_model_test.cpp` |
| pty 端到端测试 | `tests/test_s9.py` |
| 13 套测试 | `ctest` **13/13** |

---

## 2. 结构：三个层次

```text
┌──────────────────────────── agent-tui ────────────────────────────┐
│  reader thread: core.read_frame() -> screen.Post(handle_frame)     │
│                                                                    │
│  UI thread: ScreenInteractive::Loop                               │
│    Renderer(input, ...) -> TuiModel::lines()/status()/approval()   │
│    CatchEvent: y/n 审批、Esc 取消、Ctrl-C 退出                      │
└────────────────────────────────────────────────────────────────────┘
                              │ stdio / NDJSON
┌──────────────────────────── agent-core ────────────────────────────┐
│  ... S1–S8 的协议、历史、工具、策略、会话 ...                       │
└────────────────────────────────────────────────────────────────────┘
```

- **`CoreProcess` 不碰 FTXUI**：只管 spawn、send、阻塞 `read_frame()`。
- **读取线程只做搬运**：读到帧就 `screen.Post([&, f]{ handle_frame(f); })`；
  所有 UI/状态修改都在主循环线程，避免 FTXUI 的线程安全问题。
- **`TuiModel` 不碰终端**：它只消费协议帧，产出 `lines/status/approval`；
  单元测试不需要 pty，也不需要 FTXUI。
- **FTXUI 只做渲染和按键**：判定仍在 core；TUI 只负责"问"和"回传"。

---

## 3. 交互

| 输入 | 行为 |
|---|---|
| `Enter` | 发送当前输入（空行忽略；`/quit` 退出） |
| `Esc` | 审批中 = deny；跑 turn 中 = `turn.cancel`；空闲 = 吞掉 |
| `Ctrl-C` | 退出（先 shutdown，500ms 后仍活着就 SIGTERM） |
| `y` / `n` | 审批弹窗允许 / 拒绝 |
| 方向键 | 输入框内移动光标（FTXUI Input 自带） |

UI 组件：

- 顶部标题 + 分隔线；
- 消息区：user 青色、assistant 默认、tool 黄色、approval 品红、error 红色、system dim；
  只渲染最后 `dimy-8` 行，避免溢出；
- 输入行：`> ` + FTXUI Input（单行）；
- 状态栏：`status | session <id> | turns N`；
- 审批弹窗：`dbox` 覆盖，显示工具、参数、原因、`[y]/[n]/[Esc]`。

---

## 4. 事件映射（TuiModel）

| 协议 | UI |
|---|---|
| `assistant.delta` | 追加到当前 step 的 assistant 行；step 变化就另起一行 |
| `tool.call` | 加一条 `[tool] name {args}`，状态栏 `tool: name` |
| `tool.result` | 加 `-> ok/error` 行 |
| `context.compacted` | 加 `[context] compacted ~a -> ~b` 行 |
| `approval.requested` | 弹窗 + 一条轻量 transcript 行 |
| `chat response` | busy=false；若最后没有 delta，补播 `result.text`（非流式） |
| 错误 response | `[error] code: message` 行 |

---

## 5. 测试

- `tests/tui_model_test.cpp`（18 项）：不链接 FTXUI，穷举状态转换。
- `tests/test_s9.py`（20 项）：用 Python `pty` 起真实 `agent-tui`：
  - demo 模式：启动、输入、流式回答、Ctrl-C 退出码 0；
  - 工具：`read_file` 的 tool.call/结果在屏幕上可见；
  - 审批：越界写弹窗，`n` 后不写文件；
  - 分层：agent-tui 不 include core、不碰 httplib/openssl、只链 `agent_common + FTXUI`。
- `ctest`：**13/13**（S1–S8 十一套 + TuiModel 单元 + TUI pty）。

---

## 6. 踩坑

- **FTXUI CMakeLists 无条件 add_subdirectory(examples/doc)**：只复制 include/src 后配置会红；
  已在 vendored 副本里注释掉这两行，并写进 `third_party/ftxui/VENDORED.md`。
- **FTXUI 不是线程安全的**：所有状态修改都通过 `screen.Post` 回主循环；
  读取线程绝不直接碰 `TuiModel` 或 `input`。
- **读取线程的生命周期**：退出时先 `running=false`，发 shutdown，`core.stop()`
  （关 stdin，500ms 后 SIGTERM），再 `join`；否则可能卡在 `read()`。
- **pty 测试要设窗口大小**（`TIOCSWINSZ 110x30`）：不设的话 FTXUI 可能按 0 行渲染，
  断言全部失败；`TERM=xterm-256color` / `LANG=C.UTF-8` 也要设。
- **Ctrl-C 是 FTXUI 事件，不是信号**：raw mode 下 `\x03` 走 `Event::Special`，
  必须自己 `screen.Exit()`，不能等默认 SIGINT。
- **审批弹窗不是新协议**：它只是消费 S8 的 `approval.requested` / `approval.respond`；
  S9 没有新增 wire 消息。

---

## 7. 替代方案（没选）

| 方案 | 为什么没选 |
|---|---|
| 系统 FTXUI / FetchContent | 构建机离线；FetchContent 会让"clone 下来就能 build"失效 |
| ncurses | 头文件也没装；而且 FTXUI 的组件模型更适合这个 UI |
| 纯 ANSI 手写 | 可行，但要自己处理 Unicode 宽度、按键解析、重绘；FTXUI 已经解决了 |
| 不起线程（poll stdin + core） | FTXUI 的 `ScreenInteractive` 自己占主循环；用读取线程 + `Post` 最简单 |
| 把 TUI 逻辑塞进 Renderer | 状态散在 lambda 里没法单测；拆出 `TuiModel` 才能穷举 |

---

## 8. DoD

- [x] 流式输出时键盘仍可输入；Esc 能取消
- [x] `tool.call` / `tool.result` / `approval.requested` 在 UI 可见且可操作
- [x] 审批弹窗 allow/deny 都能回传（pty 测 deny；allow 路径同 `approval.respond`）
- [x] cli 保留；S1–S8 十一套仍全绿
- [x] `protocol.md` §9 S9、本文件、刷新 `HANDOFF.md`、打 tag `s9`
