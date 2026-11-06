# HANDOFF —— 会话交接与进度真相

> **新会话第一件事：读本文件 + `git log --oneline --decorate`。**
> 本文件只回答四件事：**现在在哪、下一步做什么、怎么验证、有哪些坑**。
> 阶段结束或上下文将尽时必须刷新它——它比聊天记录长寿。

---

## 1. 现在在哪（快照）

| 项 | 值 |
|---|---|
| 仓库 | `/root/project/yxAgent`（服务器 `111.228.29.170`） |
| 分支 | `main` |
| HEAD | tag `s9.2` 指向的提交 |
| tag | `s0` `s1` `s2` `s3a` `s3b` `s4` `s5` `s6` `s7` `s8` `s9` `s9.1` `s9.2` |
| 测试 | `ctest` **14/14 全绿**：S1 12 / S2 33 / S3a 20 / S3b 74 / SSE 24 / S4 51 / S5 24 / S6 单元 30 / S6 端到端 31 / S7 10 组 107 项 / S8 11 组 56 项 / S9 TuiModel 18 + textwrap 6 + TUI 20 |
| 工作区 | 干净（`.cache/`、`build/`、`model/` 已 gitignore） |
| 正在做 | 无。S9 已收尾、已打 tag，下一个阶段是 S10 |

**真实模型验证过的**：DeepSeek `deepseek-chat` 流式（11 增量、首字 0.57s）、工具循环
（read_file → wc -l → 答 "5"，3 步 2.7s）。S5 的取消、S6 的压缩、S7 的 resume、S8 的审批、
S9 的 TUI 是用**假 server / pty** 验的，真模型下的手动冒烟还没做（见 §6 欠账）。

## 2. 30 秒恢复

```bash
cd /root/project/yxAgent
git log --oneline --decorate | head -5
cmake --build build 2>&1 | tail -3
cd build && ctest 2>&1 | tail -4     # 必须 14/14
```

全绿 = 基线完好，直接从 §5 开始。真实模型冒烟（**记得给 `--tools`**，默认不给工具）：

```bash
export DEEPSEEK_API_KEY=$(head -1 /tmp/deepseek.key.bak)   # 或 model/deepseek.md
printf '你好\n' | ./build/agent-cli --base-url https://api.deepseek.com --model deepseek-chat
printf '读一下 a.txt，统计行数\n' | ./build/agent-cli --tools all --workdir /tmp/s4cli \
       --base-url https://api.deepseek.com --model deepseek-chat

# S7：同一会话关掉再开；agent-run 是一次性 headless
printf '第一句\n' | ./build/agent-cli --session demo --session-dir /tmp/s7sessions \
       --base-url https://api.deepseek.com --model deepseek-chat
printf '第二句\n' | ./build/agent-cli --resume --session-dir /tmp/s7sessions \
       --base-url https://api.deepseek.com --model deepseek-chat
./build/agent-run --tools all --workdir /tmp/s4cli --session demo --session-dir /tmp/s7sessions \
       --base-url https://api.deepseek.com --model deepseek-chat "统计 a.txt 的行数"
```

## 3. 文档地图（只查一处）

| 文件 | 回答什么 |
|---|---|
| `README.md` | 项目是什么、怎么跑、阶段进度、铁律 |
| `docs/protocol.md` | 线上到底发什么帧 + 每阶段追加的不变量 |
| `docs/responsibilities.md` | 谁拥有什么职责、逐阶段代码地图 |
| `docs/stage-s2..s9.md` | 这些阶段**为什么**这么设计、替代方案、踩的坑 |
| `docs/roadmap.md` | 阶段计划与 DoD（S0–S11；S10 的细节在那一节） |
| `docs/manual.md` | **v1 设计稿**：架构/协议/选型的"为什么"。它的阶段划分**已作废**，别照它学 |
| **本文件** | **现在在哪、下一步、怎么验证、有哪些坑** |

> S0/S1 没有单独的设计记录：S0 的设计就是 `protocol.md` 本身，S1 的记录在
> `responsibilities.md` 的「S1 代码地图」。S2 起每阶段一份 `stage-sN.md`。

## 4. 已完成（不要重做，也不要破坏）

| tag | 交付物 | 关键不变量（后续阶段必须继续满足） |
|---|---|---|
| `s0` | `protocol.md` + golden fixture | 帧形状：`{type,seq,name/payload,id/method/params}` |
| `s1` | fork + 双管道 + 握手 | 第一帧永远是 `event hello`；每个请求恰好一个 response |
| `s2` | 事件流 / `--log-raw` / `--embedded` | response 是本轮**最后**一帧 |
| `s3a` | 真模型（非流式）、历史在 core | 失败的一轮**整轮**回滚（`history_mark`） |
| `s3b` | SSE 增量解析 + 流式 | `result.text` == 本轮 delta 顺序拼接；未知/坏帧丢弃而不崩 |
| `s4` | 工具 + ReAct 循环 | `tool.call`/`tool.result` 按 `call_id` 一一配对；`arguments` 坏 JSON 不许抛 |
| `s5` | `turn.cancel`（穿透到 HTTP）+ 30ms 合批 | 取消**不回滚**已产出的部分；吐过字不重试 |
| `s6` | 上下文预算与三级压缩 + `context.compacted` | 压缩只动 `history_`，不改协议；`history_mark` 跟着往下挪 |
| `s7` | 会话日志 + `session.load`/resume + `agent-run` | 会话文件只追加；恢复重建 `history_`（含 `[压缩]`）；带会话 `seq` 续号；一个会话一个写者；`agent-run` 只靠协议 |
| `s8` | `Policy` + `approval.requested`/`approval.respond` + cli y/n | 判定在 core；ask 必须往返；deny 是工具失败不是 turn 失败；`--approval=never` 不发事件；`agent-run` 无人值守不挂起 |
| `s9` | `agent-tui`（FTXUI vendored v5.0.0）+ `TuiModel` | 读取线程只 `Post`，状态只在 UI 线程；TUI 只消费事件、不执行工具；审批走 S8；Esc 取消 / Ctrl-C 退出 |
| `s9.1` | TUI 换行修复（`textwrap`） | 按显示宽度逐 glyph 换行；中文/长 JSON 不横向溢出；消息/状态/审批弹窗共用 |
| `s9.2` | UTF-8 安全截断（`common/utf8.hpp`） | 按字节 resize 不许切汉字；tool cap/摘要/read_file/run_shell/错误 snippet 都走完整码点边界 |

## 5. 下一步（S10：MCP 接入，~400 行）

**目标**：接一个外部 MCP server（stdio），把它的工具**并入**注册表；
模型看到的是和内置工具一样的 `ToolSpec`，调用走同一条 `tool.call` / `tool.result`。

交付物（细节读 `docs/roadmap.md` 的 S10 节）：

1. MCP stdio client：spawn server -> `initialize` / 能力协商 -> `tools/list` -> `tools/call`。
2. 工具适配：MCP tool schema -> `ToolSpec`，MCP result -> `ToolResult`；命名冲突要吵。
3. 配置：`--mcp` / 配置文件；server 的 stderr 不能污染协议 stdout。
4. S8 的 Policy 对 MCP 工具同样生效（按工具名 + 参数判定）。
5. 假 MCP server 测试；S1–S9 十三套仍全绿（新增 `tests/test_s10.py`）。

DoD：

- [ ] 接一个真实 MCP server（如 filesystem），对话中能调用它的工具
- [ ] MCP server 中途死掉 / 返回坏 JSON 不拖垮 core
- [ ] 工具名冲突、参数 schema 错误都有可行动错误
- [ ] `protocol.md` §9 S10、`docs/stage-s10.md`、刷新本文件、打 tag `s10`

## 6. 欠账（明确推掉的，别忘）

- **只读工具并行、写工具串行**：需要真正的线程模型（或 `fork` + 非阻塞 `waitpid` 调度）。
  S5/S6 都保持单线程。做之前先回答"谁能写 stdout"。
- **`--embedded` 不支持取消**：同步回调、单线程，没有"等响应时还能读输入"的窗口。
- **真模型的取消 / 压缩冒烟**没做（只跑了假 server）。
- **模型摘要**（真让 LLM 写摘要）：S6 只做了抽取式，见 `docs/stage-s6.md` §5。
- **S7 的崩溃恢复是 lean 版**：只在轮末 `session.commit`，轮中 SIGKILL 会丢掉整轮
  （包括 user 消息）。DSH 式"模型请求前 / 工具副作用前 checkpoint + `TOOL_OUTCOME_UNKNOWN`"
  留给后续；做之前先想清楚审批结果怎么和 commit 对齐。
- **S8 的 Policy 是策略闸门，不是 OS 沙箱**：不改进程权限、不做 namespace/seccomp；
  shell 里的编码/注入仍可能绕过黑名单。真沙箱留给后续，别把 deny 宣传成内核级保证。
- **`--embedded` 不支持审批**：同步回调里没有等回答的窗口，ask 直接 deny（事件仍发）；
  要交互审批用 pipe 模式的 cli/tui。
- **TUI 只渲染最后 `dimy-8` 行**：没有滚动回看 / 鼠标 / 搜索；S9 先保证输入、流式、工具、
  审批四个交互对，历史滚动留给后续。
- **FTXUI 的 `text()` 不换行、`paragraph()` 只按空格切**：中文/长 JSON 会横向溢出；
  统一走 `src/tui/textwrap.*`（按 `string_width` 逐 glyph 换行），新增内容别再直接 `text(长字符串)`。
- **FTXUI 固定 vendored v5.0.0**：离线构建的代价；升级要重跑 pty 测试和 `VENDORED.md`。

## 7. 坑与约束（踩过的，别再踩）

**环境 / 构建**

- **key 只在服务器** `model/deepseek.md`（`/model/` 已 gitignore）；绝不出现在命令行（`ps` 可见）。
- 同时设 `DEEPSEEK_API_KEY` 和 `OPENAI_API_KEY` 会告警并优先前者——换厂商前先 `unset` 另一个。
- 提交前先看 `git branch --show-current`：出现过 detached HEAD 提交。
- **tag 只增不改**；旧阶段的 bug 在 `main` 的 tip 修，要跑旧阶段验收就 backport 出 `sX.Y`。

**协议 / 不变量**

- **模型是无状态的**：每次请求都要把整段 `messages` 重发。系统提示词在 `history_[0]`，
  不是每次现拼；**只给 `tools` 数组不够**——那只说明"有哪些函数"，不说"什么时候该用"。
  没工具时**不加** system（S1–S3b 的假 server 会数消息条数）。
- `assistant.delta` 必须带 `step`；response 必须最后到；未知/坏帧丢弃而不崩。
- `demo.chat` **不合批**：S2 的验收是"一个 delta 恰好一个码点"，新阶段不许偷偷改老契约。

**HTTP / 流式**

- **cpp-httplib 的 `Client::Post` 没有 ContentReceiver 重载**（只有 `Get`）。要边收边解析，
  得自己拼 `httplib::Request` + `req.content_receiver` + `cli.send(req,res,err)`；
  这条路上 `res.body` 是空的，错误正文要自己攒。
- **流式默认没有 `usage`**：请求体要带 `stream_options.include_usage`；对端不给就显示"未知"，**不许编 0**。
- **重试只覆盖传输层失败，且仅当本轮还没吐过任何 delta**（吐过再重试 = 用户看到重复的半句话）。
- 主动断流时**先判 `aborted` 再判 `!sent`**（掐流时客户端一定返回 false + `Canceled`）。

**工具**

- `tool_calls` 按 `index` 累积（分片可能乱序、`name` 也会被切开）；`arguments` 拼完才解析，
  坏 JSON 是常态：要变成一次 `ok=false` 的工具结果，**不许抛异常**。
- `run_shell` 自带超时看门狗（模型一句 `sleep 1000` 就能冻住整个 turn）；stdout/stderr 合一根管子
  （两根管子会死锁，也丢顺序）。

**取消 / 合批**

- **要 `poll` 的东西不能让 stdio 帮它缓冲**：cli 第一版用 `getline(FILE*)` + `poll(fd)`，
  后面的行被读进 stdio 缓冲区，`poll` 再也不亮 → `test_s2` 30s 超时。改用裸 fd + 自己的行缓冲。
- **取消时"先收下再喊停"**：provider 在调 `on_delta` 之前已经把这一片算进 `result.text`；
  引擎不接就返回 false 会使结算文本比增量拼接多一个字——**不变量 2 当场破**。

**压缩**

- 压缩会改 `history_` 的长度，**`history_mark` 必须跟着往下挪**（否则失败回滚的 `resize` 会把历史撑大）。
- **压缩不许把历史变大**：摘要的固定开销在小对话里可能超过原文，压完要量一下，不划算就放弃。
  副作用是短对话永不压缩——那是**正确行为**。
- **"检索类结果永不压缩"只能按轮实现**：只留 `tool` 消息、折叠掉它上面的 `assistant(tool_calls)`
  就是孤儿 tool 消息 → 对端 400。
- 摘要识别靠前缀 `[压缩]`；旧摘要要"搬运正文"，不能再折叠一次（套娃会让 token 反而上升）。

**会话 / 持久化（S7）**

- **会话文件不是 `--log-raw` 的替身**：`session.*` 记录才是恢复真相，帧只做审计和 seq 水位。
  system prompt 和 `[压缩]` 摘要都**不在线上帧里**，别想着从帧重放 history。
- **只有 `event` 帧带 `seq`**，`session.*` 不带；同一个文件里 seq 必须唯一且严格递增。
- **一轮一行 commit**（原子提交）；失败轮只写 `session.abort`，不写消息。压缩过的轮用
  `checkpoint` 全量替换，不再追加——S6 的 `history_` 是可变 vector，不是 DSH 的 surface 投影。
- **一个会话一个写者**（`flock`）。第二个进程是 `SESSION_BUSY`，不是"两份日志合并"。
- `session.load` 的请求帧不落盘（切换会话时它属于旧会话）；response 落到新会话。
- `tests/fake_llm_tools.py` 的 `messages_full` 是 S7 **追加**的字段；老行为不许动。

**TUI（S9）**

- **FTXUI 不是线程安全的**：读取线程只能 `read_frame` + `screen.Post`，绝不直接改 `TuiModel`。
- **Ctrl-C 在 raw mode 下是事件**（`Event::Special("\x03")`），不是默认 SIGINT；必须自己 `screen.Exit()`。
- **退出顺序**：`running=false` -> send shutdown -> `core.stop()`（关 stdin / 500ms 后 SIGTERM）-> `join`；
  反过来会卡在 `read()`。
- **pty 测试必须设窗口大小**（`TIOCSWINSZ`），否则 FTXUI 按 0 行渲染，断言全红。
- **FTXUI 的 CMakeLists 无条件 add_subdirectory(examples/doc)**：只复制 include/src 时要注释掉，
  见 `third_party/ftxui/VENDORED.md`。

**权限 / 审批（S8）**

- **判定在 core，前端只回 allow/deny**：前端不能绕过策略直接执行工具。
- **deny 是工具失败，不是 turn 失败**：`tool.result.ok=false` + `ERROR:` 回灌，模型自己纠正。
- **`approval.requested` 在 `tool.call` 之后、`tool.result` 之前**；approval 的 ack 是 mid-turn
  response，本轮 chat response 仍是最后一帧。
- **workdir 必须先规范化**（`weakly_canonical` + `relative`），否则 `../` / 符号链接能逃逸。
- **黑名单会误报**（`echo "rm -rf"` 也 ask）；宁可多问，不要漏。
- **`agent-run` 默认 `--approval=never`**；即使显式 ask，也会自动 deny，不能挂起。

**截断 / UTF-8**

- **JSON 只接受合法 UTF-8**：任何 `resize(n)` / `substr(0,n)` 都可能切出半个汉字，
  下一次 `json.dump()` 抛 `type_error.316`。统一用 `proto::utf8_prefix_bytes()`。
- 典型现场：tool result 截到 2048 字节 + 追加 `"\n[已截断...]"` → `0x0A` 成了非法续字节。

**测试**

- `test_s3a.py::test_layering` 是**按字面 grep 源码**的：连注释里出现 `httplib` 都会红。
  想不红就去掉那个词（写"HTTP 客户端"），别改测试。
- 单元测试的 `check(name, cond, detail)` 里 **detail 是无条件求值的**：别在里面写会越界的 `substr`。
- `tests/fake_llm.py`（S3a）/ `fake_llm_sse.py`（S3b）/ `fake_llm_tools.py`（S4）是那几阶段的验收基线，
  只许**追加**模式，不许改已有行为。

## 8. 上下文预算策略（本文件存在的理由）

- **每阶段结束必刷新本文件**，别等上下文将尽。
- 上下文快满时：**先写本文件，再结束会话**——"先写遗嘱再断电"。写清：正在做什么、卡在哪、下一条命令。
- 新会话别只说"继续"：说「读 `docs/HANDOFF.md`，从 §5 继续」。
- **不要指望聊天记录**：新会话看不到它；仓库才是持久记忆。

## 9. 怎么更新本文件（30 秒模板）

1. §1 的 HEAD / tag / 测试数字（跑一遍 §2 就知道）。
2. §4 追加一行（新 tag + 交付物 + 新增的不变量）。
3. §5 换成下一个阶段的清单（当前阶段没做完就原样保留）。
4. §7 追加本阶段新踩的坑（一条就够，别写成流水账）。

**不要**把设计理由抄进来——那是 `docs/stage-*.md` 的活；本文件只放"现在 / 下一步 / 验证 / 坑"。