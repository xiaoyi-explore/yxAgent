# S8 —— 权限、审批与沙箱（tag `s8`）

> **一句话**：模型要写 workdir 外的文件、跑 `rm -rf`、用解释器执行一段代码时，
> core 先过一道 allow / ask / deny 的闸门；ask 走协议问前端，前端只负责"问"和"回传"。
>
> **注意**：S8 是**策略闸门**，不是 OS 沙箱。它不改变进程权限、不做文件系统隔离；
> 真正的沙箱（namespace / seccomp / 容器）留给后续。别把 "deny" 当成内核级保证。

---

## 1. 交付物

| 交付物 | 落点 |
|---|---|
| `Policy`：按工具 + 参数判 allow / ask / deny | `src/core/policy.hpp/.cpp` |
| `approval.requested` 事件 + `approval.respond` 方法 | `CoreEngine` + protocol §9 S8 |
| 危险命令黑名单 / workdir 边界 / 超时 | `policy.cpp` + `request_approval()` |
| cli 行内 y/n | `src/cli/main.cpp` |
| `agent-run` 无人值守自动拒绝 | `src/run/main.cpp` |
| 审批结果进会话日志 | 既有 `log()` 帧 + `session.commit` 里的 `tool` 消息 |
| 11 组测试 | `tests/test_s8.py`，`ctest` **11/11** |

---

## 2. 策略模型

`Policy::check(tool, args)` 只回答三件事之一：

| 判定 | 含义 | 谁来执行 |
|---|---|---|
| `Allow` | 可以直接跑 | core |
| `Ask` | 需要人在环路 | 前端回 `approval.respond` |
| `Deny` | 直接拒绝，不问 | core 把错误回灌给模型 |

### 规则

| 工具 | 条件 | 默认 |
|---|---|---|
| `read_file` / `write_file` | 规范化后的路径在 workdir 内 | `Allow` |
| `read_file` / `write_file` | 路径在 workdir 外（含 `../`、绝对路径、符号链接逃逸） | `Ask` |
| `run_shell` | 命中危险命令规则 | `Ask` |
| `run_shell` | 普通命令（`echo` / `printf` / `sleep` / `wc` …） | `Allow` |
| 未知工具 | 交给 `ToolRegistry` 回 `ok=false` | `Allow` |

危险命令规则（`std::regex`，对 lowercase 命令匹配）：

- `sudo` / `su` / `doas`（提权）
- `rm -r` / `rm -f` / `rm -rf`（递归/强制删除）
- `mkfs` / `fdisk` / `parted` / `shutdown` / `reboot` / `poweroff`（系统级）
- `dd ... of=/dev/...`（裸设备写）
- `bash -c` / `sh -c` / `sh -lc`（解释器 + 代码）
- `python -c` / `python3 -c` / `node -e` / `perl -e` / `ruby -e` / `php -r`
- `curl ... | sh` / `wget ... | sh`（下载管道执行）
- `> /` / `>> /`（写根路径）
- `chmod ... /` / `chown ... /`（根路径权限）

**"解释器 + 一段代码"永不 allow-list**（Codex `exec_policy.rs` 的结论）：
只要命令里出现 `bash -c` / `python -c` / `node -e`，静态分析就失效，
必须走 `Ask` 或 `Deny`。

### 模式

| `--approval` | `Ask` 规则变成 | 用途 |
|---|---|---|
| `ask`（默认） | 发 `approval.requested` 等前端 | 交互 |
| `never` | `Deny`，不发事件 | 无人值守 / 保守 |
| `allow` | `Allow`，不问 | CI / 沙箱内；**危险，显式 opt-in** |

`--approval-timeout-ms` 默认 120000；ask 等待超时 = deny，不是 turn 失败。

---

## 3. 审批往返

```text
core                                                front-end
 |  event tool.call {call_id,name,arguments}          |
 |  event approval.requested {approval_id,reason} --> |
 |                                                    |  显示 / 问 y-n
 | <-- request approval.respond {approval_id,decision}|
 |  response {ok:true, decision} -------------------> |  ack
 |                                                    |
 |  allow: 执行工具 -> event tool.result               |
 |  deny : 不执行   -> event tool.result ok=false      |
 |  ... 最后才是本轮 chat response（不变量 1）          |
```

- **判定在 core**：前端只能回答 allow/deny，不能改策略、不能绕过。
- **deny 不是 turn 失败**：它是一次 `ok=false` 的工具结果，模型会看到
  `ERROR: approval denied: ...` 并自己纠正。
- **超时/取消也是 deny**：分别回 `approval timed out ...` / `cancelled during approval`。
- **`--embedded`**：同步回调里没有"等回答"的窗口，ask 直接 deny，事件仍会发出；
  这是已登记的例外（和 `--embedded` 不支持取消同一类）。
- **重复/错 id 的回答**：`NO_PENDING_APPROVAL`；非法 decision：`INVALID_PARAMS`。

---

## 4. 与 S5 / S7 的关系

### S5 取消

审批等待期间 `turn.cancel` 仍然生效：`request_approval()` 每 10ms 调一次
`pump_input()`，看到 `cancel_requested_` 就返回 deny（原因 `cancelled during approval`），
工具不执行；下一轮循环按 S5 的取消语义结算。历史里会留下
`ERROR: approval denied: cancelled during approval` 的 tool 消息，配对不破。

### S7 会话日志

- `approval.requested` / `approval.respond` 都是线上帧，`log()` 会自动落盘（审计）。
- 审批结果本身通过 `tool.result` 事件 + `role:"tool"` 的 `ERROR: ...` 消息
  进入 `session.commit`，所以恢复后模型知道"上次这个工具被拒绝了"。
- `session.*` 记录格式不变；审批不需要新的恢复类型。

---

## 5. cli 与 agent-run

### cli（默认 `ask`）

```text
[approval] write_file {"path":"/tmp/s8-outside.txt","content":"..."}
           原因：path is outside the working directory: /tmp/s8-outside.txt
           允许执行？[y/N]
```

- `y` / `yes` / `allow` -> allow；其他（含 EOF）-> deny。
- 提示走 stdout（用户看得见），工具进度仍走原有 stdout/stderr 规则。
- `--approval=never` 不弹；`--approval=allow` 不弹且直接放行。

### agent-run（默认 `never`）

- 默认 `--approval=never`：危险操作直接 deny，不等人，不会挂起。
- 即使显式 `--approval=ask`，agent-run 也会在收到 `approval.requested` 时
  立刻回 `deny`，并在 stderr 打 `[approval] ... -> deny (headless)`。
- 要放行危险命令只能显式 `--approval=allow`（CI / 沙箱内）。

---

## 6. 测试

`tests/test_s8.py` 11 组、**56 项 PASS**：

| # | 用例 | 验证 |
|---|---|---|
| 1 | workdir 内写 | 直接 allow，不打扰用户 |
| 2 | 越界写 ask -> allow | 事件字段齐、工具执行、文件真的写了 |
| 3 | 越界写 ask -> deny | 工具不执行、文件不存在、turn 正常结算 |
| 4 | `rm -rf` deny + 会话日志 | 不删目标；日志里有 approval.requested 和 ERROR 工具消息 |
| 5 | `--approval=never` | 不发事件、直接 deny |
| 6 | `--approval=allow` | 危险命令直接执行 |
| 7 | 超时 | 250ms 后 deny，目标不删，turn 正常结算 |
| 8 | 协议错误 | 错 id -> `NO_PENDING_APPROVAL`；非法 decision -> `INVALID_PARAMS`；正确 deny ack |
| 9 | cli y/n | y 写到外面；n 不写 |
| 10 | agent-run | 默认 never / ask 自动 deny 都不挂起 |
| 11 | 路径穿越 | `../` 按 workdir 外处理并拒绝 |

`ctest`：**11/11**（S1–S7 十套 + S8 一套）。

---

## 7. 踩坑与边界

- **`Policy` 不是沙箱**：它只挡"模型明确点名的工具参数"，挡不住 shell 里的
  `eval`、编码后的命令、环境变量注入。要真隔离，需要 OS 级沙箱（S9+）。
- **正则黑名单会误报**：`echo "rm -rf"` 也会 ask。宁可多问一次，不要漏一次；
  但别把它宣传成"完整命令分析"。
- **workdir 必须先规范化**：`weakly_canonical` + `relative` 检查，否则 `../` 和
  符号链接都能逃逸。测试 11 就是钉这个。
- **审批等待不能忙等**：单线程 + `pump_input()` + 10ms sleep；不要起线程，
  否则"谁能写 stdout"会立刻变成新问题。
- **approval ack 是 mid-turn response**：它不是本轮 chat 的 response，
  S2 的"response 是本轮最后一帧"仍然指 chat response。
- **`--approval=allow` 是危险开关**：只该在外部沙箱/CI 用；默认永远不是它。

---

## 8. 替代方案（没选）

| 方案 | 为什么不选 |
|---|---|
| 前端自己问、自己执行 | 前端被绕过就失守；判定必须在 core |
| 所有工具都 ask | 交互噪音太大；read a.txt / printf 也要问，没法用 |
| 只靠黑名单 | 解释器 + 代码让静态匹配失效；workdir 边界是另一条独立防线 |
| 真正的 OS 沙箱 | 需要 namespace/seccomp/容器，超出 S8 的 ~400 行；S8 先把"语义"做对 |
| 在 `--embedded` 里递归读 stdin | 会和 cli 的 REPL 抢输入；明确拒绝更诚实 |

---

## 9. DoD

- [x] `rm -rf` / 越界 write 被 deny（或 ask 后 deny），模型收到可行动错误
- [x] ask 时前端往返一次，allow 后工具执行；deny 不执行
- [x] `agent-run` 在无人值守下不挂起（默认 never；ask 自动 deny）
- [x] 审批结果进会话日志；S1–S7 十套仍全绿（新增 `tests/test_s8.py` → 11/11）
- [x] `protocol.md` §9 S8、本文件、刷新 `HANDOFF.md`、打 tag `s8`
