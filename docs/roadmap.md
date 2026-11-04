# yxAgent 学习路径 v2 —— 12 个更小、更线性的阶段（S0–S11）

> 本文档**取代** `manual.md` 第四章「九个阶段」。
> 手册其余章节（架构、协议细节、调研来源）依然有效，本文只重排**学习顺序与每阶段的负担**。
> 读者画像：有 3 年 C/C++ 经验，用过 ChatGPT/Claude，但从未写过 agent，也不了解 LLM API 的底层细节。

---

## 一、为什么要重排

原 S0 要求一次交付 5 样东西：`docs/protocol.md`、`agent-core`、`agent-cli`、`--embedded` 开关、`--log-raw` 抓包。
对新手来说，第一天就同时面对：进程管理、NDJSON 分帧、事件序列号、握手时序、CMake 多目标——**至少 4 个未知数同时上桌**，违反了学习路径的基本原则：

> **一次只调一个未知数。**（Anthropic: "find the simplest solution possible, and only increase complexity when needed"）

v2 的改法：

1. **S0 变成 0 行代码**——只写协议文档。协议先行，但"先行"不等于"和代码一起上"。
2. **cli 先行，TUI 靠后**。先用 20 行的 stdin/stdout 小程序当前端，TUI（FTXUI）推到 S9——那时已有真实的流式 delta、工具调用、审批弹窗可画，而不是对着假数据做界面。
3. **先能 chat，再成为 agent**。S3a 末（里程碑 M1）`./agent-cli` 就能和真模型聊天（整段返回），S3b 末（M2）变成真·流式；S4 才引入工具循环——那一刻你才真正写出 Agent。
4. **调试工具随复杂度引入**：`--log-raw` 在管道第一次变复杂时（S2）引入；`--embedded` 在接真模型后排查问题时（S2）引入。不提前。
5. **每个阶段只加 1~2 个文件**，每阶段都给出"你现在会了什么"的自检清单。

---

## 二、总览：12 个阶段与 4 个里程碑

| 阶段 | tag | 一句话 | 新增概念（≤3） | 新增文件 | 代码量 |
|---|---|---|---|---|---|
| S0 | `s0` | 只写协议，不写代码 | NDJSON 分帧、请求/响应/事件三分法、seq 序列号 | `docs/protocol.md` | 0 |
| S1 | `s1` | core 和 cli 握手成功 | 进程边界、spawn+握手、echo 回显 | core + cli 两个最小二进制 | ~250 |
| S2 | `s2` | 管道里跑通假对话流 | 事件流、`--log-raw`、`--embedded` | core 内置 demo 模型 | ~250 |
| S3a | `s3a` | **里程碑 M1：cli 能真 chat（非流式）** | Provider 抽象、HTTP POST、messages/role 模型 | provider + httplib | ~300 |
| S3b | `s3b` | **里程碑 M2：真·流式打字机** | SSE 分帧、分片边界、错误与重试 | sse 解析器 + 假 LLM server | ~450 |
| S4 | `s4` | **里程碑 M3：成为 Agent** | 工具循环、注册表、tool result 回灌 | Tool 接口 + 3 个工具 | ~600 |
| S5 | `s5` | 打断与并发 | 取消语义、节流、读写分离并发 | —— | ~400 |
| S6 | `s6` | 上下文不爆 | token 预算、摘要压缩、结果裁剪 | ContextManager | ~500 |
| S7 | `s7` | 会话能恢复 | transcript 落盘、resume、headless 前端 | `agent-run` | ~400 |
| S8 | `s8` | 不会乱来 | 策略模型、审批往返、危险命令黑名单 | Policy 模块 | ~400 |
| S9 | `s9` | **里程碑 M4：TUI 诞生** | 事件驱动 UI、状态分离、线程安全交接 | `agent-tui` | ~700 |
| S10 | `s10` | 工具可插拔 | MCP stdio 协议、能力协商、工具并入注册表 | mcp client | ~400 |
| S11 | `s11` | **里程碑 M5：完整 agent** | 子智能体上下文隔离、技能加载 | task 工具 + skills | ~500 |

合计约 5300 行，分 13 次消化，平均每次 400 行左右——**每个阶段都是"一个晚上 + 一个周末下午"能消化的体量**。

> **一次真实的估算修正**：S2 原计划 ~250 行，实测 ~600 行（事件流 + Session 抽象 + 引擎抽离三件事挤在一起）。
> 所以 S3 拆成 **S3a（先不流式）** 与 **S3b（升级成 SSE）**——每个阶段只引入一个未知数，体量也可控。
> tag 用 `s3a` / `s3b`，设计记录分别是 `docs/stage-s3a.md` / `docs/stage-s3b.md`。

里程碑（学习过程中的成就感锚点）：

- **M1（S3a 末）**：`./agent-cli` 启动，输入问题，收到真模型的**整段**回复——它终于是个聊天机器人了。
- **M2（S3b 末）**：同一句话改成**逐字流式**出现；而且中文不会乱码（分片切在码点中间也不怕）。
- **M3（S4 末）**：说"读一下 a.txt 数几行"，它自己调工具完成——Agent 诞生。
- **M4（S9 末）**：打开 `./agent-tui`，界面观感向 Claude Code / Hermes 靠拢。
- **M5（S11 末）**：能接 MCP、能派子任务、主上下文不被污染。

---

## 三、core 与 cli 各自需要什么（S1 之后读）

S1 结束时把这一节写成 `docs/responsibilities.md`，之后每阶段改动了职责边界就更新它。

### agent-core（大脑，拥有全部"真相"）

| 职责 | 为什么在 core | 什么时候引入 |
|---|---|---|
| 与 LLM 的 HTTP/SSE 通信 | 前端不该知道 HTTP 的存在 | S3a/S3b |
| 消息历史（真相状态） | 换前端不丢对话 | S3a |
| 工具注册表与执行 | 权限判定必须和执行同进程 | S4 |
| Agent 循环（turn 状态机） | 循环是协议语义，不是 UI 语义 | S4 |
| 取消与节流 | 取消要穿透到 HTTP 层 | S5 |
| token 预算与压缩 | 同上 | S6 |
| 会话持久化 | 落盘格式是协议的一部分 | S7 |
| 审批判定（allow/ask/deny） | 前端被绕过也不能失守 | S8 |

### agent-cli / agent-tui（脸，只消费事件）

| 职责 | 为什么在前端 | 什么时候引入 |
|---|---|---|
| stdin/stdout 行式收发（S1–S8 的最简形态） | 调试期最少未知数 | S1 |
| 把 `assistant.delta` 渲染成打字机效果 | core 从不发"渲染好的字符串" | S2 起 |
| 收集用户输入 → `user.message` 请求 | —— | S1 |
| 审批弹窗（只负责"问"和"回传答案"） | 决策在 core | S8 |
| TUI 主循环与事件队列 | FTXUI 的线程模型要求 | S9 |

### 铁律（每阶段 DoD 里都检查）

- `agent_common` 静态库**绝不链接** HTTP 库或 TUI 库。
- core 从不向前端发"渲染结果"，只发结构化事件。
- 前端**不认识** HTTP / 工具 / 上下文这些词。

---

## 四、逐阶段详解

### S0 —— 纯协议设计（0 行代码）

| 项 | 内容 |
|---|---|
| **交付物** | 只有 1 个文件：`docs/protocol.md` |
| **新增概念** | NDJSON 分帧、请求/响应/事件三种消息、seq 单调序列号 |
| **验证** | 文档里每条消息都有**一行真实报文示例**；只看文档能说清"握手是谁先谁后" |
| **刻意不做** | 不定义任何 LLM、工具相关的消息——留到对应阶段再补 |

**这一阶段你写什么**：不是代码，是**消息字典**。最少需要：

```
请求（cli → core，要回复）：  {"type":"request","id":"r1","method":"hello","params":{}}
响应（core → cli，对应请求）：{"type":"response","id":"r1","ok":true,"result":{}}
事件（core → cli，不要回复）：{"type":"event","seq":1,"name":"assistant.delta","payload":{}}
```

加上 4 条生命周期消息：`hello` / `welcome` / `ping` / `shutdown`。就这些。
协议文档是**活文档**，每个阶段只追加一节，永不推翻重写。

**为什么值一个晚上**：双进程架构能不能活下去，取决于协议先行。现在纸上多想 10 分钟，S3 就少熬一晚。

---

### S1 —— core 与 cli 握手（~250 行）

| 项 | 内容 |
|---|---|
| **交付物** | `agent-core`（能被 spawn，回 echo）+ `agent-cli`（20 行 stdin/stdout 小程序） |
| **新增概念** | 进程边界、spawn + stdio 管道、握手时序 |
| **验证** | `echo hello | ./agent-cli` 打出 core 的 echo 回复 |
| **刻意不做** | 假模型、日志抓包、embedded 模式、TUI |

**cli 是什么**：一个 `while (std::getline(std::cin, line))` 循环，把每行包成 NDJSON 请求写进 core 的 stdin，从 core 的 stdout 读一帧打印。**没有界面，没有历史，没有状态**——它只是一根管子。

**core 是什么**：一个读 stdin、写 stdout 的死循环，收到 `hello` 回 `welcome`，收到 echo 请求原样回显。

**本阶段结束时写 `docs/responsibilities.md`**（见第三章），把两个进程各自"欠什么债"列清楚。

**你现在会了**：两个进程怎么生、怎么死、怎么一问一答。仅此而已，但这就是双进程架构的全部骨架。

---

### S2 —— 管道里跑通假对话流（~250 行）

| 项 | 内容 |
|---|---|
| **交付物** | core 内置 `demo.chat` 假模型（逐字吐 delta）；`--log-raw` 开关；`--embedded` 开关 |
| **新增概念** | 事件流（一个请求 → 多个事件 → 一个响应）、日志抓包、同进程直连调试 |
| **验证** | cli 里输入一句话，看到打字机效果（假的）；`--log-raw` 能看到双向完整报文；`--embedded` 输出与管道模式完全一致 |
| **刻意不做** | 真模型、HTTP |

**为什么先上假模型**：S3 接真模型后，"卡了"会有三种可能——HTTP 挂了、SSE 解析挂了、渲染挂了。先用假模型把**管道本身**跑通，S3 出问题时就能确定"是 HTTP 的锅"。**一次只调一个未知数**从这里开始兑现。

**`--log-raw`**：core 把双向每一帧原样追加到文件。NDJSON 天生可读，出问题时直接翻日志。

**`--embedded`**：cli 不 spawn 子进程，进程内直连 core 接口。双进程出问题时，用它一次就能判断"是协议/传输的锅，还是逻辑的锅"。（对应 Codex 的 `InProcessAppServerClient` / `RemoteAppServerClient`。）

---

### S3a —— 接上真模型：先不流式（~300 行）⭐ 里程碑 M1

| 项 | 内容 |
|---|---|
| **tag** | `s3a` |
| **交付物** | `Provider` 接口 + `OpenAICompatProvider`（`POST /chat/completions`，`stream:false`）；`--base-url --api-key --model` 配置；新增 `chat` 方法；**假 LLM server**（返回完整 JSON） |
| **新增概念** | Provider 抽象、messages/role 模型、HTTP 客户端接入、错误与重试 |
| **验证** | `./agent-cli` 启动、打字、收到真模型**整段**回复；base-url 指向假 server 也能过（CI 确定性） |
| **刻意不做** | 流式（SSE）、工具 |

**为什么先不流式**：流式是"HTTP + 分片边界 + 编码"三重难度的叠加。S3a 只引入 **HTTP/JSON** 这一个未知数——
core 里把 `demo_reply()` 换成 `provider_->chat(messages)`，**帧形状一个字都不用改**（S2 已经定好了）。

**消息历史在这一阶段进 core**：多轮对话逼着你把 `messages` 数组管起来——它是 S6 压缩、S7 落盘的原材料。

**铁律检查**：httplib 必须挡在 `agent_core_lib` 之外（`docs/responsibilities.md` 里那条"已登记的例外"）。

---

### S3b —— 升级成 SSE 流式（~450 行）⭐ 里程碑 M2

| 项 | 内容 |
|---|---|
| **tag** | `s3b` |
| **交付物** | SSE 增量解析器；`stream:true`；`assistant.delta` 改为来自真模型；中途断连/重试；假 LLM server 回放 `tests/fixtures/*.sse` |
| **新增概念** | SSE 分帧（与 NDJSON 分帧对照）、分片边界、保活注释行、UTF-8 码点切断、重试 |
| **验证** | 真·流式打字机；下面六条 fixture 全部通过 |
| **刻意不做** | 工具（S4 才做） |

**S2 与 S3b 的分工**（这是本阶段最该记住的一句话）：
**S2 定"事件流长什么样"，S3b 定"怎么从 HTTP 分片里还原出这些事件"。**

SSE 解析的正确算法（出问题时按此排查，原手册保留）：

1. buffer 追加每个 chunk；**绝不假设 chunk 落在事件边界**。
2. 找完整事件终止符（空行），先归一化 `\r\n` / `\n` / `\r`。
3. `:` 开头是注释（keep-alive ping）要忽略；多个 `data` 行用换行拼接。
4. 派发后 `erase` 已消费前缀——**不要 `clear()` 整个 buffer**。

**假 LLM server 在这一阶段长全**：基于 `httplib::Server` 回放 `tests/fixtures/*.sse`。必覆盖 fixture：

| fixture | 验证什么 |
|---|---|
| 正常多轮 | 基线 |
| **chunk 切在 JSON 事件中间** | TCP/TLS 会任意分片，绝不能假设一次 read = 一个事件 |
| **chunk 切在 UTF-8 码点中间** | 中文流式必踩；S2 的"一个 delta 一个码点"就是这件事的预演 |
| `:` 开头的注释行 | OpenAI 的 keep-alive ping，必须忽略 |
| 慢速流（事件间 sleep 50ms） | 事件确实是一点一点到的，不是攒完一次吐 |
| 401 / 429 / 500 / 中途断连 | 错误处理与重试 |
---

### S4 —— Agent 循环与工具调用（~600 行）⭐ 里程碑 M3

| 项 | 内容 |
|---|---|
| **交付物** | `Tool` 接口 + `read_file` / `write_file` / `run_shell`；工具结果回灌消息历史；turn 状态机 |
| **新增概念** | tool-calling 循环（ReAct）、注册表、JSON Schema 参数、循环硬上限 |
| **验证** | "读 a.txt 并统计行数"全自动完成；协议文档追加 `tool.call` / `tool.result` 事件 |

```
loop:
  resp = provider.chat(messages, tools)
  if 无 tool_calls: 输出最终答复，turn 结束
  for each call:
      emit tool.call
      result = registry.execute(call)     ← 结构化 ok/output/error
      messages += tool_message(call.id, result)
      emit tool.result
  if steps > MAX_STEPS: 中止
```

**最大的坑**（务必先知道）：流式响应里 `tool_calls[].function.arguments` 是**分片到达**的，必须按 `index` 累积拼接，等 `finish_reason == "tool_calls"` 才能解析。配套 fixture：arguments 分 3 片到达。这是 C++ Agent 客户端最常见的 bug。

---

### S5 —— 打断与并发（~400 行）

| 项 | 内容 |
|---|---|
| **交付物** | `turn.cancel` 请求；`assistant.delta` 节流（~30ms 合批）；只读工具并行、写工具串行 |
| **新增概念** | 取消语义（穿透到 HTTP 层）、背压与节流、线程模型 |
| **验证** | 长回答期间发送 cancel 能立刻停下；多个 `read_file` 并行执行 |

取消**不能只设标志位**，要能中断 SSE 读取本身。取消语义由 core 定义，不由前端定义。

---

### S6 —— 上下文管理、预算与压缩（~500 行）

| 项 | 内容 |
|---|---|
| **交付物** | `ContextManager`：token 估算 → 阈值检测 → 摘要压缩 → `context.compacted` 事件 |
| **新增概念** | token 预算、compaction、工具结果裁剪、context rot |
| **验证** | 灌超长对话，自动压缩后仍能答对早期事实 |

可直接抄的参数（Gemini CLI 实测值）：阈值 = 上限 **50%**；保护最近 **3 轮**；检索类工具结果**永不压缩**；老工具结果截断到 **2KB**。

**必知概念**：context rot（Anthropic）——上下文越长，模型召回越差。压缩不只是省钱，更是保效果。

---

### S7 —— 会话持久化、恢复与 headless（~400 行）

| 项 | 内容 |
|---|---|
| **交付物** | `~/.agent/sessions/<id>.jsonl` 追加写；`session.load/resume`（按 seq 补齐）；新增 `agent-run` 一次性 headless 前端 |
| **新增概念** | transcript 落盘、resume、传输可替换 |
| **验证** | 关掉 cli 再开，历史还在；`agent-run "统计行数"` 无界面完成同一任务 |

到这里双进程的收益真正兑现：**同一 core，两个前端**（cli 和 agent-run），零改动。

---

### S8 —— 权限、审批与沙箱（~400 行）

| 项 | 内容 |
|---|---|
| **交付物** | `Policy`（按工具+参数决定 allow/ask/deny）；`approval.requested` 事件 + cli 里 y/n 行内确认；工作目录限制；shell 超时 |
| **新增概念** | 策略模型、人在环路审批、危险命令黑名单 |
| **验证** | 写工作目录外的文件被拒或需审批；`rm -rf` 类命令必须显式确认 |

两条工业界结论直接抄：

1. **"解释器 + 一段代码"永不 allow-list**：`bash -c`、`sh -lc`、`python -c`、`node -e`、`sudo`、`rm` …（Codex `exec_policy.rs`）
2. **审批判定在 core**：tui/cli 只负责问和回传。前端被绕过也不能失守。

注意：此刻还没有 TUI，审批用 cli 的行内 `y/n` 交互——**先有审批语义，S9 再给它一张脸**。

---

### S9 —— TUI 诞生（~700 行）⭐ 里程碑 M4

| 项 | 内容 |
|---|---|
| **交付物** | `agent-tui`（FTXUI）：输入框 + 消息列表 + 流式打字机 + 状态栏 + 审批弹窗；cli 保留作调试前端 |
| **新增概念** | 事件驱动 UI、视图状态与真相状态分离、线程安全交接 |
| **验证** | 对话体验向 Claude Code / Hermes 靠拢：流式不卡键盘、Esc 取消、工具调用可视、审批弹窗可用 |

**为什么 TUI 放这么晚**：此时它消费的是**真实**的 delta、tool.call、approval.requested 事件——每个界面元素都对应一个你已经理解的事件。原方案在 S1 就画 TUI，只能对着假数据调布局，学不到东西还占时间。

FTXUI 线程安全交接（关键 API，原手册保留）：

```cpp
// IPC 线程收到一帧后：
screen.Post([&]{ messages.push_back(msg); });   // 任务在主循环线程执行
```

---

### S10 —— MCP 接入（~400 行）

| 项 | 内容 |
|---|---|
| **交付物** | MCP stdio client：连接外部 MCP server，把它的工具**并入**注册表；协议文档追加 MCP 节 |
| **新增概念** | MCP 能力协商、stdio 子进程、外部工具与内置工具的统一 |
| **验证** | 接入一个真实 MCP server（如 filesystem），对话中能调用它的工具 |

你会发现 MCP server 就是一个"别人写的 agent-core 工具包"，而你在 S1 已经写过一模一样的 spawn + NDJSON 握手——**这就是协议先行的回报**。

---

### S11 —— 子智能体、技能与收尾（~500 行）⭐ 里程碑 M5

| 项 | 内容 |
|---|---|
| **交付物** | `task` 工具（独立上下文的子 Agent，只回收结论）；技能文件加载；结构化日志与 usage 统计 |
| **新增概念** | 子智能体（上下文隔离）、渐进式披露的技能系统 |
| **验证** | "调研 → 汇总"任务由子 Agent 完成，主上下文不被污染 |

子智能体放最后，因为它**给每个 bug 乘以 2**。走到这里，你已经有一台会聊天、会用工具、会审批、会记忆、能扩展的完整 agent——和 Claude Code / Hermes 同构，只是规模不同。

---

## 五、每阶段统一 DoD（沿用 v1）

1. `cmake --build` 干净通过，无警告。
2. 有自动化测试（S3a 起由假 LLM server 驱动）。
3. `docs/protocol.md` 同步追加本阶段消息。
4. `docs/responsibilities.md` 若职责有变则更新。
5. 打 tag，README「学习路径」表登记。
6. **写一句"本阶段我调试时最痛的一件事"**——这是你私人的、最值钱的笔记。
7. 更新 `docs/HANDOFF.md`（进度真相：现在在哪、下一步、怎么验证）——给"下一个会话的你"看。
8. 写/更新 `docs/stage-sN.md`（为什么这么设计、替代方案、踩了什么坑）。

---

## 六、与原 9 阶段的映射

| 原（v1） | 新（v2） | 变化 |
|---|---|---|
| S0 双进程骨架与协议 | **S0 + S1 + S2** | 拆三份：纯协议 → 握手 → 假流+调试工具。embedded/log-raw 延后 |
| S1 TUI 外壳（假数据） | 取消独立阶段 | demo.stream 并入 S2；真 TUI 推迟到 S9，画真数据 |
| S2 接上 LLM | **S3a + S3b** | 先非流式接上真模型（M1），再升级为 SSE 流式（M2） |
| S3 Agent 循环 | **S4** | 不变，里程碑 M3 |
| S4 流式取消并发 | **S5** | 不变 |
| S5 上下文压缩 | **S6** | 不变 |
| S6 持久化多前端 | **S7** | headless 提供第二个前端，TUI 不再是前置 |
| S7 权限审批 | **S8** | 审批先做行内 y/n，弹窗留给 S9 |
| S8 子智能体 MCP | **S10 + S11** | MCP 提前于子智能体（注册表是 MCP 的地基，先有地基） |

**一句话记住 v2**：先纸上谈兵（S0），再搭管子（S1–S2），接上电（S3a）、让它自己流动（S3b），装上手脚（S4），然后慢慢教它刹车（S5）、记性（S6）、笔记（S7）、规矩（S8）、脸面（S9）、外交（S10）、分身（S11）。