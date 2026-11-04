# manual —— 从零实现一个「简化但完整」的 Agent

> **目标**：用 C++ 实现一个 TUI Agent，代码量控制在可通读范围内，但**架构上覆盖现代 Agent 的全部核心概念**：进程边界、事件流、工具调用循环、上下文管理、压缩、权限审批、子智能体、会话持久化。
> **适用**：了解 C++，但对 Agent 内部机制和「双进程」只是刚听说的人。
> **学习方式**：全文分 **9 个阶段（S0–S8）**，每个阶段是一个 git tag，可 `git checkout` 切换，逐个阶段对照代码学习。
> **本文件性质**：**设计文档**。先评审，评审通过后再开工写代码。
> **调研基础**：所有关键决策均标注了工业界出处，完整来源见 [§十一](#十一调研来源)。核心结论已与 Codex CLI / Gemini CLI / Claude Code / OpenHands / Aider 的实际实现交叉验证。

### 一句话总纲

> **先在一个进程里把循环写对，把每个扩展点做成循环上的缝；最后再把这条缝变成 socket。**

这与你 `键盘输入与图形显示全链路.md` 的结论完全同构：**连键盘到显示器这条最成熟的链路，都是结构化事件 + 句柄，而不是 printf。**

---

## 零、先回答你的问题：为什么要双进程？

你手册里写的 s0 是「双进程实现，agent-core 和 agent-cli」。这个判断**是对的**，但理由需要讲清楚，因为「为什么要两个进程」是这份设计里最重要的一个架构决策。

### 0.1 结论先行

| 方案 | 代表 | 适不适合学习 |
|---|---|---|
| **单进程**（UI 和 Agent 循环同进程） | Aider（严格单进程）、Claude Code（单进程，内部分层） | 简单，但学不到分层 |
| **同协议双形态：core 可内嵌、可独立进程** | **Codex CLI**（`app-server-client` 同时提供 `InProcessAppServerClient` 与 `RemoteAppServerClient`） | ✅ **强烈推荐** |
| **包边界拆分**（同一进程内的库分层） | Gemini CLI（`@google/gemini-cli-core` 与 `@google/gemini-cli`） | 好，但进程边界更硬 |
| **多进程/分布式**（daemon + worker 池 + 远程控制） | OpenHands（server ↔ sandbox ↔ Action Execution Server） | 过于复杂，不适合入门 |

### ⚠️ 一个重要的调研修正

你手册里的「双进程」方向没错，但**工业界的做法比「写两个程序」更精妙**，值得你从一开始就抄对：

**Codex CLI 的做法是：协议只有一份 JSON-RPC envelope，传输层可换。**

- `app-server-client` 同时导出 `InProcessAppServerClient`（core 内嵌在同一进程）和 `RemoteAppServerClient`（连本地/远程 daemon）。
- 即使在**内嵌**路径上，响应**仍然走同一个 JSON-RPC result envelope**——没有第二条代码路径要维护。
- daemon 连不上时，TUI **自动退回内嵌 server**（`app-server-daemon/README.md`）。

**对你的建议：采用「同一套协议，两种部署形态」。**

| 形态 | 用途 | 切换方式 |
|---|---|---|
| **内嵌（embedded）** | 开发调试、单机简单场景 | `agent-tui --embedded` |
| **独立进程（split）** | 你手册 s0 要的形态、多前端 | `agent-tui`（默认 spawn core） |

**这样做的最大好处**：S0 你写的那套协议**一行都不用改**就能支持两种形态；而且当双进程出问题时，你可以用 `--embedded` 立刻判断「是协议/传输的锅，还是 Agent 逻辑的锅」——**一次只调一个未知数**。

同时，Codex 用编译期 lint 强制边界，这个技巧成本极低、收益极高，**建议直接抄**：

```rust
// codex-rs/core/src/lib.rs
#![deny(clippy::print_stdout, clippy::print_stderr)]
// "All user-visible output must go through the appropriate abstraction."
```

C++ 的对应物：让 `libs/protocol` 和 `core` 这两个 target **物理上链接不到** UI 库，并且禁止直接 `std::cout`（可用 `-Werror` + 自定义 lint，或最简单地把日志封装成唯一出口）。

**推荐：采用「同协议、双形态」，把它理解成「一个 harness + 一个前端」，而不是「两个平等的程序」。**

### 0.2 双进程到底买到了什么

| 收益 | 说明 | 在你的项目里如何体现 |
|---|---|---|
| **关注点分离** | core 只管「LLM + 工具 + 状态」，前端只管「渲染 + 键盘」 | core 里没有一行 ANSI 转义码；tui 里没有一行 HTTP |
| **崩溃隔离** | Agent 循环 panic / 工具跑飞，不会带走你的界面 | 工具执行超时被 kill，core 还能回报错误 |
| **多前端** | 同一份 core，可以挂 TUI、可以挂脚本、可以挂 Web | S5 会加一个 headless 的 `agent-run` 一次性执行 |
| **会话可持久** | core 常驻，会话在内存里活着，前端可以断开重连 | S6 的 session 恢复 |
| **可测试** | 不用起终端就能测 core；伪造一个假 LLM 就能测全链路 | S2 就有 mock provider |
| **协议即文档** | 进程边界强制你定义清晰的 IDL，而不是靠函数调用乱窜 | S0 就定下 `protocol.md` |

**核心认知**（呼应你 `键盘输入与图形显示全链路.md` 第六节的结论）：

> 现代系统里**没有任何一层把「人类可读字符串」当接口**。USB 传 HID 报告，evdev 传 `input_event`，DRM 传 ioctl。
> **core 与 tui 之间也必须是「结构化事件」，而不是 core 拼好的字符串。**

如果 tui 消费的是 core 生成的 `"正在思考...`` 这种面向人类的文本，你就退回到了 printf 架构，双进程的意义丧失一半。

### 0.3 双进程的代价（要诚实）

| 代价 | 缓解手段 |
|---|---|
| 需要定义并维护协议 | 协议**只增不改**，早期就写进 `protocol.md` |
| 序列化开销、调试更难 | 提供 `--log-raw` 把两边原始报文落盘，S0 就做 |
| 前端拿不到 core 的内存对象 | 事件流设计成**自包含**，前端不依赖 core 内部状态 |
| 启动顺序/生命周期管理 | 前端负责 spawn core，并做优雅关闭（见 §2.4） |

### 0.4 与工业界的一致性（调研依据）

- **LSP**（Microsoft）：编辑器与语言服务器分离，协议用 `Content-Length` 头分帧，跑在 stdio 或 socket 上。
- **MCP**（Anthropic）：客户端与 server 分离，stdio transport 明确规定「每条消息一行 JSON-RPC，**不得包含内嵌换行**」，并指出这套 framing「不改动即可跑在 Unix domain socket 或 TCP 上」。
- **Claude Code 2.1.169**（社区逆向分析）：早期是单 REPL 进程，新版**长出了 background daemon**——systemd/launchd 常驻、supervisor + workers、rendezvous socket。

也就是说：**「先单进程、后拆出 core」是工业界的真实演化路径，而你的手册直接跳到双进程，是抄了近路——这是好事，但要理解你抄掉的是什么。**

---

## 一、总体架构

### 1.1 分层图

```
┌──────────────────────────────────────────────────────────────┐
│  agent-tui  （前端进程：只做呈现与输入）                       │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ Input Handler   →  按键/编辑 → 结构化 Intent            │  │
│  │ Event Reducer   →  消费 core 事件 → 维护本地视图状态     │  │
│  │ Renderer        →  视图状态 → 屏幕（FTXUI）             │  │
│  └────────────────────────────────────────────────────────┘  │
└───────────────────────────┬──────────────────────────────────┘
                            │  结构化事件流 + 请求/响应
                            │  （JSON 帧，见 §二）
┌───────────────────────────┴──────────────────────────────────┐
│  agent-core （后端进程：harness，持有全部真相）                 │
│                                                              │
│  ┌────────────┐   ┌──────────────┐   ┌──────────────────┐   │
│  │ Session    │   │ Agent Loop   │   │ Provider         │   │
│  │ (transcript│──▶│ (turn 状态机) │──▶│ (LLM API, SSE)   │   │
│  │  , 分支)   │   │              │   │                  │   │
│  └────────────┘   └──────┬───────┘   └──────────────────┘   │
│                          │                                   │
│         ┌────────────────┼────────────────┐                  │
│         ▼                ▼                ▼                  │
│  ┌────────────┐  ┌──────────────┐  ┌──────────────┐         │
│  │ Tool       │  │ Context      │  │ Policy /     │         │
│  │ Registry   │  │ Manager      │  │ Approval     │         │
│  │ (内置+MCP) │  │ (预算/压缩)   │  │ (权限判定)    │         │
│  └────────────┘  └──────────────┘  └──────────────┘         │
└──────────────────────────────────────────────────────────────┘
                            │
                            ▼
                    外部世界（文件系统 / shell / HTTP / MCP servers）
```

### 1.2 与系统链路的类比（承接你已有的那份文档）

| 系统链路 | 本架构 | 阶段 |
|---|---|---|
| 键盘只报物理键 | 工具只报**结构化事实**，不做语义解释 | S3 |
| `input_event` 统一格式 | core 产出统一 `Event` 结构 | S0 |
| libinput 做布局解释 | tui 做「呈现策略」解释 | S1 |
| 合成器持有焦点与权限 | core 持有权限判定与审批入口 | S7 |
| PTY 行规程决定信号语义 | session 层决定「中断/取消」语义 | S4 |
| VBlank 节流渲染 | 流式输出的节流与背压 | S4 |
| Direct Scanout 快速路径 | headless 模式绕过 TUI | S5 |

---

## 二、通信协议设计（S0 的核心产出）

### 2.1 分帧方式

业界就两种，都值得你亲手写一遍：

| 分帧方式 | 采用者 | 形态 | 优点 | 风险 |
|---|---|---|---|---|
| **NDJSON**（一行一条 JSON） | MCP stdio、ACP stdio | `{...}\n` | 实现极简，`jq` / `tail -f` 直接可读 | **依赖序列化器不产生裸换行** |
| **Content-Length 头** | LSP、Codex app-server | 头 + `\r\n\r\n` + 正文 | **字节精确、二进制安全**；不解析 JSON 也能转发整条消息 | 多约 20 行头部解析 |

### 选择：S0 用 NDJSON，但把分帧抽成接口

**理由**：MCP 规范规定「消息由换行分隔，**MUST NOT** 包含内嵌换行」——这对你反而是好事，因为 `nlohmann::json::dump()` **默认就会转义换行**，你天然满足这个 MUST。

但 NDJSON 有个**致命的隐性风险**：一旦有人手动拼字符串（而不是走 `dump()`），或不小心输出带裸换行的内容，**整个流会静默错位，且极难定位**。所以必须加防御性断言：

```cpp
// 发送前断言：消息体内不得含裸换行
assert(line.find('\n') == std::string::npos);
```

**同时把分帧抽成接口**，为 S6 留门：

```cpp
class IFraming {
public:
    virtual ~IFraming() = default;
    virtual void write(Transport&, const json& msg) = 0;
    virtual std::optional<json> read(Reader&) = 0;   // 读满一条返回一条
};
// S0: NdjsonFraming     可选: ContentLengthFraming（照 LSP 写，约 20 行）
```

Codex 的做法值得学：它要接 MCP 时，**加了一个 `stdio-to-uds` 适配器，而不是改协议**。你的 `IFraming` 是同一个思路。

> **铁律（无论选哪种分帧）**：写到 stdout 的每一行必须是合法 JSON；**任何日志一律走 stderr**。
> 这是 MCP 规范里的 MUST，照抄即可。

### 2.2 传输层

S0 用 **stdio（父子进程管道）**，因为最简单、跨平台、无需清理 socket 文件。
但**协议层不绑定传输**——S6 增加 `--listen unix:/tmp/agent.sock`（Linux/macOS）或 `--listen pipe:agent-core`（Windows），协议完全不变。

### 2.3 消息类型

**请求/响应**（前端 → core）：
```json
{"v":1,"id":"7","type":"req","method":"session.prompt","params":{"session_id":"s1","text":"你好"}}
{"v":1,"id":"7","type":"res","ok":true,"result":{...}}
{"v":1,"id":"7","type":"res","ok":false,"error":{"code":"provider_error","message":"...","retryable":true}}
```

**事件**（core → 前端，单向，无 id）：
```json
{"v":1,"type":"event","name":"turn.started","seq":12,"session_id":"s1","data":{"turn_id":"t3"}}
{"v":1,"type":"event","name":"assistant.delta","seq":13,"session_id":"s1","data":{"turn_id":"t3","text":"你"}}
{"v":1,"type":"event","name":"tool.call","seq":14,"session_id":"s1","data":{"call_id":"c1","name":"read_file","args":{"path":"a.txt"}}}
{"v":1,"type":"event","name":"tool.result","seq":15,"session_id":"s1","data":{"call_id":"c1","ok":true,"output":"..."}}
{"v":1,"type":"event","name":"turn.finished","seq":16,"session_id":"s1","data":{"turn_id":"t3","usage":{"in":1200,"out":340}}}
```

**事件清单（S0 定稿，后续只增不改）**：

| 事件 | 语义 |
|---|---|
| `session.created` / `session.loaded` | 会话就绪 |
| `turn.started` / `turn.finished` | 一次「用户输入 → Agent 停下」的完整回合 |
| `assistant.delta` | 流式文本增量（节流后） |
| `assistant.message` | 一条完整的 assistant 消息 |
| `tool.call` / `tool.result` | 工具调用与结果 |
| `approval.requested` / `approval.resolved` | 审批（S7） |
| `context.compacted` | 发生了压缩（S6） |
| `error` | 可展示的错误 |

**关键设计**：`seq` 单调递增。前端可以据此检测丢包、并在重连时用 `session.resume`（S6）补齐。

### 2.4 生命周期

```
agent-tui 启动
   └─ fork/exec agent-core（stdin/stdout 接管道）
        └─ core 发 {"type":"event","name":"ready","data":{"protocol":1,"version":"0.1.0"}}
             └─ tui 校验版本，发 session.create
                  └─ 正常交互
关闭：tui 发 {"method":"shutdown"} → 等 core 退出 → 超时则 kill
```

---

## 三、技术选型

| 关注点 | 选择 | 理由 |
|---|---|---|
| 语言标准 | **C++20** | `std::format`、`jthread`、`<ranges>` 都能省代码 |
| 构建 | **CMake ≥ 3.20 + vcpkg**（或 FetchContent） | 依赖管理最省心 |
| JSON | **nlohmann/json 3.12.0** | 头文件库、API 最友好、`dump()` 天然转义换行 |
| HTTP | **cpp-httplib 0.60.x**（C++11 单头文件，带 OpenSSL 支持） | 内建 **SSE 客户端**（`httplib::sse::SSEClient`）与流式 content receiver，省掉手写 SSE 解析 |
| TUI | **FTXUI 7.x** | MIT、C++17、**零外部依赖**、声明式 Element 树、内建事件循环 |
| 并发 | `std::jthread` + 一条 IO 线程 + 一条 Agent 线程 | 学习阶段不引入 Asio |
| 测试 | Catch2 / doctest + **假 LLM server** | 见 §五 |

**几个选型要点（已核实）**：

- **SSE 不用自己解析**：`cpp-httplib` 的 `sse::SSEClient` 提供 `on_message` / `on_event` / `start_async()` / `stop()`，带自动重连与 `Last-Event-ID` 续传。**但它的 `Result` 不是线程安全的**，且流式请求**没有 keep-alive**（每次一个独立连接）——所以必须在**单条专用线程**上跑。
- **JSON 全项目统一用 nlohmann/json**：它在「工具调用 arguments 字符串 ↔ 对象」的往返上最顺手。simdjson 更快，但要求 `padded_string` 且是**一次性迭代器**，对一个每轮只解析几 KB 的 Agent 属于过度设计。包一层 `json_io.hpp`，将来要换也只改一个文件。
- **TUI 用 FTXUI 是无争议的**：零外部依赖、跨平台、CMake 友好，且提供了我们需要的线程安全交接 API。
- **IPC 传输**：S0 用 **stdio 管道**（最简、生命周期由 OS 管）；S6 若要换传输，注意**只有 TCP loopback（127.0.0.1）是真正三平台字节一致的代码**——UDS 在 Windows 上（108 字节路径上限、支持不完整）没有可靠故事。因此把传输封装在 `Channel` 接口之后，**序列化层加长度前缀**，让上层完全不感知传输差异。

**为什么 TUI 不选 ncurses/notcurses**：FTXUI 把「渲染」和「状态」分成 Dom 层（纯函数、立即模式）和 Component 层（保留模式、带事件），这个分层**正好对应**你要学的「core 出事件、tui 维护视图状态」——用它来教概念，比 ncurses 的裸 API 直观得多。

**为什么 HTTP 不用 Boost.Beast**：Beast 是异步的、学习曲线陡。入门阶段用同步 + 独立线程，**先让流程跑通，再谈性能**。

**关于 SSE 的一个省力点**：cpp-httplib 自带 `httplib::sse::SSEClient`，提供 `on_message` / `on_event` 回调、`start_async()` 后台线程模式、自动重连与 `Last-Event-ID` 续传。S2 直接用它，**把精力留给 Agent 逻辑而不是协议解析**。（S4 的取消语义仍需自己接：`ssse.stop()` 可在回调外中断连接。）

---

## 四、九个阶段（git tag 可直接 checkout）

> 每个阶段都遵循：**能跑 → 能看 → 能测**。tag 命名 `s0`…`s8`。
> 「新增概念」一栏就是这一阶段你**应该学会的东西**。

### S0 —— 双进程骨架与协议

| 项 | 内容 |
|---|---|
| **tag** | `s0` |
| **新增概念** | 进程边界、协议先行、NDJSON 分帧、事件序列号 |
| **交付物** | `docs/protocol.md`；`agent-core` 能 spawn、握手、回显；`agent-cli` 极简（无 TUI，纯 stdin/stdout）；**`--embedded` 开关**（同进程直连，不经管道） |
| **验证** | `echo 'hi' \| ./agent-cli` 与 `echo 'hi' \| ./agent-cli --embedded` **输出完全一致**；`--log-raw` 能看到双向报文 |
| **刻意不做** | LLM、工具、TUI |

**关键点**：`--embedded` 不是花架子，它是你的**调试利器**——当双进程出问题时，用它一次就能判断「是协议/传输的锅，还是 Agent 逻辑的锅」，**一次只调一个未知数**。这正是 Codex `InProcessAppServerClient` / `RemoteAppServerClient` 的设计意图。

这是你手册里 s0 的忠实实现，但**多了两样东西**：一份协议文档，和一个内嵌模式。协议先行是双进程架构能否活下去的关键。

---

### S1 —— TUI 外壳（还是假数据）

| 项 | 内容 |
|---|---|
| **tag** | `s1` |
| **新增概念** | 事件驱动 UI、视图状态与真相状态分离、流式节流 |
| **交付物** | FTXUI 界面：输入框 + 消息列表 + 状态栏；`agent-cli` 改名 `agent-tui` |
| **验证** | core 内置 `demo.stream` 命令，逐字吐 delta；UI 打字机效果不卡键盘 |
| **关键点** | tui 只消费事件，**不认识 HTTP / 工具 / 上下文** |

**要刻意体会的**：core 从来没有给 tui 发过「渲染好的字符串」。tui 收到的是 `assistant.delta`，它自己决定怎么画。

**FTXUI 的线程安全交接点（关键 API）**：TUI 主循环跑在一条线程上，而 IPC 读帧跑在另一条线程。**绝不能在 IPC 线程里直接改 UI 状态**。正确做法：

```cpp
// IPC 线程收到一帧后：
screen.PostEvent(ftxui::Event::Custom);   // 线程安全：入队并唤醒主循环
// 或携带数据：
screen.Post([&]{ messages.push_back(msg); });   // 任务在主循环线程执行，无数据竞争
```

注意 FTXUI 中 `ScreenInteractive` 现在是 `App` 的别名（`using ScreenInteractive = App;`），API 在 `App` 上。

---

### S2 —— 真正接上 LLM（Provider 抽象）

| 项 | 内容 |
|---|---|
| **tag** | `s2` |
| **新增概念** | Provider 抽象、SSE 流式解析、消息模型（role/content）、错误与重试 |
| **交付物** | `Provider` 接口 + `OpenAICompatProvider`（可指向 OpenAI / DeepSeek / 本地 vLLM）；SSE 增量解析；`agent-core --base-url` 配置 |
| **验证** | 与模型完成一次多轮闲聊；CI 里用**假 LLM server** 保证确定性 |
| **关键点** | **还没有工具**。此时它是一个「聊天机器人」，不是 Agent |

**假 LLM server 是本阶段最重要的工程动作**：一个基于 `httplib::Server` 的本地 server，回放 `tests/fixtures/*.sse` 脚本。有了它，之后每个阶段都能自动化测试。

**必须覆盖的 fixture（每一条都对应一类真实 bug）**：

| fixture | 验证什么 |
|---|---|
| 正常多轮 | 基线 |
| **chunk 切在 JSON 事件中间** | TCP/TLS 会任意分片，**绝不能假设一次 read 等于一个事件** |
| **chunk 切在 UTF-8 码点中间** | 中文流式输出必踩；多字节字符被劈成两半 |
| `:` 开头的注释行 | OpenAI 的 keep-alive ping，必须忽略 |
| **tool_call arguments 分 3 片到达** | 见 S3 的坑 |
| 慢速流（事件间 sleep 50ms） | TUI 节流逻辑与界面不卡 |
| 401 / 429 / 500 / 中途断连 | 错误处理与 SSE 重连 |

### SSE 增量解析的正确算法（无论用不用库都要懂）

用到 `cpp-httplib` 的 `SSEClient` 时它会帮你解析，但**理解这个算法**能让你在出问题时知道去哪找：

1. 维护一个 `std::string buffer`，把每个到达的 chunk **原样追加**。
2. **绝不假设 chunk 落在事件边界上**——一次 read 里可能有半个事件，也可能有三个半。
3. 在 buffer 里找**完整的事件终止符**（空行）。注意规范允许 `\r\n`、`\n`、`\r` 三种行尾，要先归一化。
4. 块内按 `field: value` 处理：冒号后的一个前导空格要剥掉；`:` 开头的行是注释（OpenAI 的 ping）**要忽略**；多个 `data` 行是**用换行拼接**而不是替换。
5. 在空行处派发事件，然后 **`erase` 掉已消费的前缀——不要 `clear()` 整个 buffer**，否则会丢掉下一个事件的开头。

> 权威定义：[WHATWG HTML Standard §9.2 Parsing an event stream](https://html.spec.whatwg.org/multipage/server-sent-events.html)

---

### S3 —— Agent 循环与工具调用（真正的 Agent 诞生）

| 项 | 内容 |
|---|---|
| **tag** | `s3` |
| **新增概念** | ReAct / tool-calling 循环、工具注册表、JSON Schema 参数、结果回灌、循环终止条件 |
| **交付物** | `Tool` 接口 + 3 个工具：`read_file`、`write_file`、`run_shell`；消息历史里正确插入 tool 结果；`turn` 状态机 |
| **验证** | 「读 a.txt 并统计行数」能自动完成；单轮内多次工具调用的多步推理 |
| **关键点** | 循环必须有**硬上限**（最大步数 / 最大 token / 墙钟超时），否则会失控 |

```
Agent Loop（turn 内部）
  loop:
    resp = provider.chat(messages, tools)
    if resp 无 tool_calls:  → 输出最终答复，turn 结束
    for each call:
        emit tool.call
        result = registry.execute(call)   ← 结构化结果，不是字符串
        messages += tool_message(call.id, result)
        emit tool.result
    if steps > MAX_STEPS → 中止并报错
```

**呼应 §1.2 的第一条**：`registry.execute` 返回的是带 `ok/output/error` 的**结构化事实**；把它渲染成人话是 tui 或 prompt 模板的事。

### ⚠️ S3 最容易踩的坑（务必先知道）

**OpenAI 兼容接口的 `tool_calls[].function.arguments` 是分片到达的。**

流式响应里，工具调用的参数 JSON **不是一个完整字符串**，而是被切成多个 delta 片段：

```
chunk1: {"index":0,"function":{"name":"read_file","arguments":"{\"pa"}}
chunk2: {"index":0,"function":{"arguments":"th\":\"a.tx"}}
chunk3: {"index":0,"function":{"arguments":"t\"}"}}
```

必须**按 `index` 累积拼接**，直到 `finish_reason == "tool_calls"` 才能 `json::parse`。直接对单个 delta 解析会抛异常。

这是 C++ Agent 客户端里**最常见的一个 bug**。配套的测试用例（见 S2 的假 LLM server）：准备一个 fixture，让 `arguments` 分 3 片到达，断言拼完能解析。

---

### S4 —— 流式、取消与并发

| 项 | 内容 |
|---|---|
| **tag** | `s4` |
| **新增概念** | 背压与节流、取消语义、线程模型、工具并发执行 |
| **交付物** | `assistant.delta` 节流（~30ms 合批）；`turn.cancel` 请求；只读工具并行、写工具串行；core 内部无阻塞 IO |
| **验证** | 长回答期间按 Esc 能立刻停下；多个 `read_file` 并行 |
| **关键点** | 取消要能穿透到 HTTP 层（中断 SSE 读取），不能只设置一个标志位 |

这是「PTY 行规程决定中断语义」的对应物：**取消语义由 session 层定义，不由 UI 层定义**。

---

### S5 —— 上下文管理、预算与压缩

| 项 | 内容 |
|---|---|
| **tag** | `s5` |
| **新增概念** | token 预算、上下文窗口、压缩（compaction）、工具结果裁剪、token 估算 |
| **交付物** | `ContextManager`：token 估算 → 阈值检测 → 摘要压缩 → 发 `context.compacted` 事件；大工具结果截断 |
| **验证** | 灌入超长对话，观察自动压缩后仍能回答早期事实 |
| **关键点** | 压缩是**有损**的，必须保留：用户原始意图、当前任务状态、未完成事项 |

**工业界的两种压缩策略**（都要知道，实现一种即可）：

| 策略 | 代表 | 做法 | 特点 |
|---|---|---|---|
| **摘要（summarize）** | Gemini CLI `ChatCompressionService` | 达阈值后让模型总结，保留最近 N 轮完整 | 保信息，但多一次模型调用 |
| **重置（reset）** | Codex `compact_token_budget.rs` | **不做摘要**，直接装一个全新上下文窗口 | 快、省 token，但丢历史 |

**可以直接抄的具体参数**（Gemini CLI 实测值）：
- 阈值 = 模型上限的 **50%**（`DEFAULT_COMPRESSION_TOKEN_THRESHOLD = 0.5`）——**不要等到 95% 才压**。
- **保护最近 3 轮**完整不动（`RECENT_TURNS_PROTECTED = 3`）。
- **检索类工具的返回永不被压缩**（`read_file`、`grep`、`glob` 等）——否则模型会反复重读同一文件。
- 较老的工具返回**截断到 2KB**（`COLLAPSED_FUNCTION_RESPONSE_MAX_BYTES = 2048`）。

**天花板在哪**：Claude Code 2.1.169 的压缩已是多路径状态机——自动、微型（清理工具结果）、**预计算**（用户还在打字时后台先算好摘要）、反应式，外加**熔断器**（连续失败则停止尝试）与**快速回填断路器**（上下文反复迅速涨满时抑制压缩）。

**你的简化版只需做三件事**：阈值检测 + 摘要压缩 + 保护最近 N 轮。但要知道天花板在哪。

**另一个必知概念——context rot**（Anthropic）：上下文越长，模型的**召回能力越差**，且这在所有模型上都存在。所以压缩不只是「省钱」，更是**保效果**。

---

### S6 —— 会话持久化、恢复与多前端

| 项 | 内容 |
|---|---|
| **tag** | `s6` |
| **新增概念** | transcript 落盘、会话恢复、传输可替换、headless 模式 |
| **交付物** | `~/.agent/sessions/<id>.jsonl` 追加写；`session.load` / `session.resume`（按 `seq` 补齐）；`agent-core --listen unix:...`；新增 `agent-run` 一次性 headless 前端 |
| **验证** | 关掉 tui 再打开，历史还在；`agent-run "统计行数"` 无 UI 完成同一任务 |
| **关键点** | 到这里，**双进程的全部收益才真正兑现**——同一 core，两个前端 |

headless 前端是「Direct Scanout 快速路径」的对应物：简单场景绕过完整 UI 渲染。

---

### S7 —— 权限、审批与沙箱

| 项 | 内容 |
|---|---|
| **tag** | `s7` |
| **新增概念** | 能力/策略模型、人在环路审批、沙箱、危险操作分级 |
| **交付物** | `Policy`：按工具 + 参数模式决定 `allow / ask / deny`；`approval.requested` 事件与 tui 弹窗；工作目录限制；shell 超时与资源限制 |
| **验证** | 写工作目录外文件被拒或需审批；危险命令需显式确认 |
| **关键点** | 审批入口在 **core**，不在 tui。tui 只负责「问」和「回传答案」 |

这是「合成器持有权限判定」的对应物。**权限真相必须和工具执行在同一进程**，否则前端被绕过就失守了。

**三个工业界结论，直接决定你的设计：**

1. **沙箱的作用是「减少审批弹窗」，而不只是「防破坏」。** Claude Code 的思路：命令一旦被 OS 级沙箱限住（只能写工作目录、网络走白名单代理），就可以**不再逐条问用户**。体验和安全的矛盾由此化解。你的 S7 应该实现「沙箱内的命令自动放行，沙箱外的命令才 ask」。

2. **必须有一份「永不放行」的名单。** Codex 的 `exec_policy.rs` 明确列出**禁止被 allow-list 化的命令前缀**——因为它们能绕过一切静态检查：
   `bash -c`、`sh -lc`、`python -c`、`node -e`、`powershell -EncodedCommand`、`cmd /c`、`sudo`、`rm` …
   **这条规则值得直接抄**：任何「解释器 + 一段代码」的组合，静态策略都不可靠。

3. **沙箱有明确的覆盖盲区。** Claude Code 官方文档写得很直白：沙箱**只**约束 shell 命令；**内置文件工具、MCP server、hooks 都跑在沙箱之外**，且原生 Windows 不支持（须 WSL2）。

**对应到你的设计**：S7 的 `Policy` 应该对**每一个工具**做判定（而不只是 shell），并在文档里明确写出「哪些路径不受沙箱保护」。

---

### S8 —— 子智能体、技能与收尾

| 项 | 内容 |
|---|---|
| **tag** | `s8` |
| **新增概念** | 子智能体（上下文隔离）、技能/提示词模块化、MCP 接入、可观测性 |
| **交付物** | `task` 工具：把子任务交给一个**独立上下文的子 Agent**，只回收结论；技能文件加载；MCP client（把 MCP server 的工具并入注册表）；结构化日志与 usage 统计 |
| **验证** | 一个「调研 → 汇总」任务用子 Agent 完成，主上下文不被污染；接入一个真实 MCP server |
| **关键点** | 子智能体的**唯一目的**是上下文隔离——它用掉很多 token，但只把摘要还给你 |

**工业界对照**：Claude Code 2.1.169 的子系统清单里，Agent Teams、Workflows（VM 沙箱内的确定性多智能体编排）、MCP（stdio/SSE/HTTP/WebSocket + OAuth）、Loops & Scheduling 都已是独立子系统。**S8 之后的路还很长，但你已经走完了主干。**

---

## 五、阶段总览表

| 阶段 | tag | 一句话 | 新增核心概念 | 代码量（估） |
|---|---|---|---|---|
| S0 | `s0` | 双进程能对话 | 协议、分帧、事件序列 | ~600 |
| S1 | `s1` | 有 TUI 外壳 | 事件驱动 UI、状态分离 | ~700 |
| S2 | `s2` | 接上真模型 | Provider、SSE、重试 | ~800 |
| S3 | `s3` | **成为 Agent** | 工具调用循环 | ~900 |
| S4 | `s4` | 能流式能取消 | 背压、取消、并发 | ~600 |
| S5 | `s5` | 上下文不爆 | 预算、压缩 | ~700 |
| S6 | `s6` | 会话能恢复、多前端 | 持久化、可替换传输 | ~700 |
| S7 | `s7` | 不会乱来 | 权限、审批、沙箱 | ~600 |
| S8 | `s8` | 会分工、能扩展 | 子智能体、技能、MCP | ~1000 |

合计约 **6600 行**——足以覆盖主干概念，又不至于读不完。

### 每阶段统一的完成定义（DoD）

1. `cmake --build` 干净通过，无警告即目标。
2. 有对应的自动化测试（假 LLM server 驱动）。
3. `docs/protocol.md` 同步更新。
4. 有一份 `docs/stage-sN.md`：这一阶段**为什么这么设计**、**替代方案是什么**。
5. 打 tag，并在 README 的「学习路径」表里登记。

### 强烈建议：transcript 录制 + 回放（S2 起）

把每一轮都记成 **JSONL**：请求体（完整 message 数组 + 工具 schema）+ 每个原始 SSE 事件 + 时间戳。因为用了 nlohmann/json，这只是几行代码。然后：

- 给 core 加一个 `replay` 模式：读录制文件，把**同样的 SSE 事件**重新灌进 IPC 通道。
- 这样就能**零网络、确定性**地测试整条 `core → tui` 管线（含 UI 流式渲染）。
- 因为格式是纯 NDJSON，它同时是一个**可以直接在终端里读的调试产物**——当故障是「UI 上某个字乱码了」时，这一点极其关键。

> **分层铁律的 CMake 落点**：`agent_common`（静态库）**绝不链接** HTTP 库或 TUI 库。
> HTTP 属于 core，渲染属于 tui——这一条规则就是双进程架构是否诚实的试金石。

---

## 六、仓库结构（终态）

```
new-agent/
├── CMakeLists.txt
├── docs/
│   ├── protocol.md            # 协议唯一真相
│   ├── stage-s0.md … s8.md    # 每阶段设计说明
│   └── manual.md    # 本文件
├── libs/
│   ├── protocol/              # 消息结构、序列化、分帧（core/tui 共用）
│   └── common/                # 日志、错误、结果类型
├── core/
│   ├── main.cpp
│   ├── session/               # transcript、恢复
│   ├── loop/                  # Agent 循环、turn 状态机
│   ├── provider/              # Provider 接口 + OpenAI 兼容实现
│   ├── tools/                 # 注册表 + 内置工具
│   ├── context/               # 预算、压缩
│   ├── policy/                # 权限、审批
│   └── rpc/                   # 传输与消息泵
├── frontends/
│   ├── tui/                   # agent-tui
│   └── run/                   # agent-run（headless，S6 起）
├── mcp/                       # S8：MCP client
└── tests/
    ├── fake_llm/              # 假 LLM server
    └── ...
```

**分层铁律**：`frontends/*` **只能** include `libs/protocol` 和 `libs/common`。
一旦前端 include 了 `core/provider`，双进程就白做了。可以在 CI 里加一条检查来强制它。

---

## 七、几个必须提前想清楚的坑

| 坑 | 后果 | 对策 |
|---|---|---|
| **协议没有版本号** | 前后端不同版本时静默错乱 | `v` 字段 + `ready` 握手校验 |
| **换行没转义** | 一行 JSON 被拆成两行，framing 崩坏 | 只用 `json.dump()`；加一个「消息不含裸换行」的断言 |
| **日志混进 stdout** | 协议流被污染，极难查 | **stdout 只许协议，日志一律 stderr**；CI 加检查 |
| **没有工具循环上限** | Agent 无限调工具烧钱 | MAX_STEPS + token 预算 + 墙钟超时，三者取先到 |
| **取消只设标志位** | 用户按了 Esc 但请求还在跑 | 取消要中断底层 socket 读 |
| **压缩丢关键信息** | Agent 忘了原始任务 | 压缩 prompt 强制保留「用户意图 / 当前任务 / 未完成事项」 |
| **权限判定放在前端** | 前端被绕过即失守 | 判定与执行同在 core |
| **工具结果是字符串** | 前端只能原样打印，无法结构化呈现 | 工具返回 `{ok, output, error, meta}` 结构 |
| **工具输出不截断** | 一次 `cat` 大文件直接撑爆上下文 | **序列化给模型之前**就集中截断（Codex `TruncationPolicy`） |
| **工具超时返回空结果** | 模型以为「命令成功但无输出」，继续错下去 | 超时/失败要**显式**回传原因，不能静默为空 |
| **审批通道阻塞协议通道** | 未读的通知把请求-响应堵死 | 通知队列**无界**，请求通道有界（Codex 明确记录的坑） |
| **前端能直连模型** | 绕过 core 的权限与上下文管理 | CI 检查前端不得依赖 provider 库 |
| **前端 include 了 core** | 架构腐蚀 | 目录隔离 + CI 检查 |

---

## 八、这份设计**刻意不做**的事

明确边界，避免范围蔓延：

- ❌ 不做多模态（图片/音频）。
- ❌ 不做 RAG / 向量库（那是另一个主题，可在 S8 后作为独立扩展）。
- ❌ 不做远程/云端会话（Claude Code 的 bridge 那样）。
- ❌ 不做多智能体编排引擎（Workflows 那层）。
- ❌ 不追求性能极致（同步 HTTP + 线程足够）。
- ❌ 不做插件的热加载/ABI 稳定性。

**目标只有一个**：把 Agent 的主干概念**每一条都亲手写一遍**。

---

## 九、建议的学习方式

1. **先读 `docs/protocol.md`**，它是整个系统的枢纽。
2. `git checkout s0`，跑起来，看 `--log-raw` 的原始报文。
3. 逐阶段 `git diff s(N-1)..sN`，**先看 diff 再看代码全文**。
4. 每阶段读完，回答 `docs/stage-sN.md` 里的「替代方案」问题：**如果换成另一种做法会怎样**。
5. 每阶段结束做一次「破坏性实验」：故意去掉循环上限、故意让日志写 stdout，**亲眼看到它怎么坏**。

---

## 十、结语：一句话记住整个架构

> **core 是唯一持有真相（transcript / 上下文 / 权限）的 harness，tui 是它的一个可替换的呈现前端，两者之间流动的永远是结构化事件而非人类文本。**

这句话如果立住了，双进程就立住了；双进程立住了，后面 8 个阶段都只是往 core 里加模块而已。

---

## 十一、调研来源

本设计的关键决策均有出处，便于你自行核实与深挖。

### 协议与分帧
- [JSON-RPC 2.0 规范](https://www.jsonrpc.org/specification) —— 三种消息类型与标准错误码，本设计 §2.3 直接沿用
- [MCP —— Transports（stdio）](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports) —— NDJSON 分帧、「MUST NOT 含内嵌换行」、stdout/stderr 纪律
- [MCP —— Lifecycle](https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle) —— initialize 握手与版本协商
- [MCP —— Tools](https://modelcontextprotocol.io/specification/2025-06-18/server/tools) —— 工具由模型控制、annotations 视为不可信
- [LSP 3.17 规范（Base Protocol）](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/) —— Content-Length 分帧的权威定义
- [Agent Client Protocol —— Transports](https://agentclientprotocol.com/protocol/transports) —— ACP 采用的同类分帧

### 进程模型（本设计最重要的调研）
- [Codex CLI —— app-server-client](https://raw.githubusercontent.com/openai/codex/main/codex-rs/app-server-client/src/lib.rs) —— **InProcessAppServerClient / RemoteAppServerClient 共用同一 JSON-RPC envelope**，§0.1 修正的依据
- [Codex CLI —— app-server-daemon README](https://raw.githubusercontent.com/openai/codex/main/codex-rs/app-server-daemon/README.md) —— daemon 生命周期、内嵌回退、环境继承的坑
- [Codex CLI —— core/src/lib.rs](https://raw.githubusercontent.com/openai/codex/main/codex-rs/core/src/lib.rs) —— `#![deny(clippy::print_stdout, clippy::print_stderr)]`，分层边界的编译期强制
- [Codex CLI —— uds crate](https://raw.githubusercontent.com/openai/codex/main/codex-rs/uds/src/lib.rs) —— 0700 套接字目录、非提权 peer 校验
- [OpenHands 后端架构](https://docs.openhands.dev/openhands/usage/architecture/backend) —— server ↔ sandbox ↔ Action Execution Server 的多进程形态
- [Aider main.py](https://raw.githubusercontent.com/Aider-AI/aider/main/aider/main.py) —— 严格单进程的对照样本

### Agent 循环与上下文
- [ReAct: Synergizing Reasoning and Acting in LLMs](https://arxiv.org/abs/2210.03629) —— S3 循环的理论源头
- [Anthropic —— Effective context engineering for AI agents](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents) —— **context rot**、上下文是有限资源，S5 的核心论据
- [Anthropic —— Building effective agents](https://www.anthropic.com/engineering/building-effective-agents) —— 「从最简单的方案开始」的方法论
- [Gemini CLI —— chatCompressionService.ts](https://raw.githubusercontent.com/google-gemini/gemini-cli/main/packages/core/src/context/chatCompressionService.ts) —— 0.5 阈值、保护最近 3 轮、检索工具不压缩、2KB 截断
- [Gemini CLI —— turn.ts](https://raw.githubusercontent.com/google-gemini/gemini-cli/main/packages/core/src/core/turn.ts) —— 事件分类与 MAX_TURNS=100、LoopDetected
- [Codex CLI —— compact_token_budget.rs](https://raw.githubusercontent.com/openai/codex/main/codex-rs/core/src/compact_token_budget.rs) —— 「重置式」压缩
- [Codex CLI —— context_manager/mod.rs](https://raw.githubusercontent.com/openai/codex/main/codex-rs/core/src/context_manager/mod.rs) —— 上下文管理实现

### 权限、沙箱与工具
- [Codex CLI —— exec_policy.rs](https://raw.githubusercontent.com/openai/codex/main/codex-rs/core/src/exec_policy.rs) —— Policy 引擎、禁止 allow-list 的命令前缀，S7 第 2 条
- [Codex CLI —— tools/mod.rs](https://raw.githubusercontent.com/openai/codex/main/codex-rs/core/src/tools/mod.rs) —— 命名空间工具名、ToolRouter、并行调度
- [Claude Code —— Sandboxing the Bash tool](https://code.claude.com/docs/en/sandboxing) —— **沙箱以减少审批为目的**、覆盖盲区、Windows 需 WSL2，S7 第 1/3 条
- [Claude Code —— 上下文窗口说明](https://code.claude.com/docs/en/context-window) —— 启动上下文的 token 预算实测
- [Claude Code —— Skills](https://code.claude.com/docs/en/skills) ｜ [Subagents](https://code.claude.com/docs/en/sub-agents) ｜ [Hooks](https://code.claude.com/docs/en/hooks) —— S8 的三条扩展缝
- [anthropics/sandbox-runtime](https://github.com/anthropics/sandbox-runtime) —— 开源沙箱实现
- [Agent Skills 开放标准](https://agentskills.io) —— 渐进披露的 SKILL.md 约定

### 前端与库（版本均于调研时核实）
- [FTXUI](https://github.com/ArthurSonzogni/FTXUI) —— 最新 **v7.0.3**；MIT、C++17、零外部依赖、Dom/Component 双层
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) —— 最新 **v0.60.1**；C++11 单头文件，内建 [SSE 客户端](https://github.com/yhirose/cpp-httplib/blob/master/README-sse.md) 与流式 content receiver
- [nlohmann/json](https://github.com/nlohmann/json) —— 最新 **v3.12.0**

### SSE 与流式解析
- [WHATWG HTML Standard §9.2 —— Server-sent events](https://html.spec.whatwg.org/multipage/server-sent-events.html) —— 事件流解析的权威规范，S2 算法出处
- [cpp-httplib —— SSE 客户端文档](https://github.com/yhirose/cpp-httplib/blob/master/README-sse.md) —— `on_message` / `start_async` / 自动重连
- [cpp-httplib —— 流式 API 文档](https://github.com/yhirose/cpp-httplib/blob/master/README-stream.md) —— 无 keep-alive、Result 非线程安全
- [cpr —— Server-Sent Events](https://github.com/libcpr/docs/blob/master/sse.md) —— `SetServerSentEventCallback` 的备选方案

### FTXUI 关键 API
- [FTXUI —— app.hpp](https://github.com/ArthurSonzogni/FTXUI/blob/main/include/ftxui/component/app.hpp) —— `Loop` / `Post` / `PostEvent` / `RequestAnimationFrame` / `Exit` / `PrintAbove`
- [FTXUI —— screen_interactive.hpp](https://github.com/ArthurSonzogni/FTXUI/blob/main/include/ftxui/component/screen_interactive.hpp) —— `using ScreenInteractive = App;` 别名

### 参考项目
- [Cursor —— Continually improving our agent harness](https://cursor.com/blog/continually-improving-agent-harness) —— 「harness 独立于模型迭代」的产品视角

---

## 十二、附：配套调研文档

本次调研产出了两份原始报告，内容比本设计文档更细，可作为补充阅读：

| 文件 | 内容 |
|---|---|
| [architecture-brief.md](./architecture-brief.md) | 进程模型、Agent 循环、Harness 组件、IPC 选项的完整调研（含 Codex/Gemini/Claude Code 源码级引用） |
| [tech-stack.md](./tech-stack.md) | C++ 技术选型详报：HTTP/SSE、JSON、TUI、IPC、构建系统、测试策略，含可直接用的 CMakeLists |

---

*文档版本：v1（设计稿，待评审）｜ 共 9 阶段（S0–S8），git tag 可切换*

