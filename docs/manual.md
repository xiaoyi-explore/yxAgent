# manual —— 从零实现一个「简化但完整」的 Agent

> ## ⚠️ 这是 v1 设计稿，只保留"为什么"
>
> 本文写于项目动工之前（2026-10 初稿）。它现在的价值只有一条：**架构、协议、选型为什么这么定**。
>
> - **阶段划分已作废**：本文原来的「九个阶段（S0–S8）」被 v2 取代。现行计划是
>   [`docs/roadmap.md`](roadmap.md) 的 **S0–S11**（S3 拆成 S3a/S3b，共 13 个 tag）。
>   **tag 名的含义一律以 roadmap 为准**——比如 v1 的 `s8` 是"子智能体"，v2 的 `s8` 是"权限审批"。
> - **协议现状以 [`docs/protocol.md`](protocol.md) 为准**：本文第二章只记录 S0 定稿那一刻的设计，
>   之后 S1/S2/S3a/S3b/S4/S5/S6 又追加了帧、事件和方法。
> - **每阶段"为什么这么做"看 [`docs/stage-*.md`](.)**，本文不再重复。

> **目标**：用 C++ 实现一个 TUI Agent，代码量控制在可通读范围内，但**架构上覆盖现代 Agent 的全部核心概念**：进程边界、事件流、工具调用循环、上下文管理、压缩、权限审批、子智能体、会话持久化。
> **适用**：了解 C++，但对 Agent 内部机制和「双进程」只是刚听说的人。
> **读法**：本文只讲**架构 / 协议 / 选型**。阶段划分与学习顺序见 [`docs/roadmap.md`](roadmap.md)（S0–S11，S3 拆成 S3a/S3b）；每阶段的实现理由见 `docs/stage-*.md`。
> **本文件性质**：**设计文档**。先评审，评审通过后再开工写代码。
> **调研基础**：所有关键决策均标注了工业界出处，完整来源见 [§十](#十调研来源)。核心结论已与 Codex CLI / Gemini CLI / Claude Code / OpenHands / Aider 的实际实现交叉验证。

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
| 测试 | Catch2 / doctest + **假 LLM server** | 见 roadmap 的 DoD |

**几个选型要点（已核实）**：

- **SSE 不用自己解析**：`cpp-httplib` 的 `sse::SSEClient` 提供 `on_message` / `on_event` / `start_async()` / `stop()`，带自动重连与 `Last-Event-ID` 续传。**但它的 `Result` 不是线程安全的**，且流式请求**没有 keep-alive**（每次一个独立连接）——所以必须在**单条专用线程**上跑。
- **JSON 全项目统一用 nlohmann/json**：它在「工具调用 arguments 字符串 ↔ 对象」的往返上最顺手。simdjson 更快，但要求 `padded_string` 且是**一次性迭代器**，对一个每轮只解析几 KB 的 Agent 属于过度设计。包一层 `json_io.hpp`，将来要换也只改一个文件。
- **TUI 用 FTXUI 是无争议的**：零外部依赖、跨平台、CMake 友好，且提供了我们需要的线程安全交接 API。
- **IPC 传输**：S0 用 **stdio 管道**（最简、生命周期由 OS 管）；S6 若要换传输，注意**只有 TCP loopback（127.0.0.1）是真正三平台字节一致的代码**——UDS 在 Windows 上（108 字节路径上限、支持不完整）没有可靠故事。因此把传输封装在 `Channel` 接口之后，**序列化层加长度前缀**，让上层完全不感知传输差异。

**为什么 TUI 不选 ncurses/notcurses**：FTXUI 把「渲染」和「状态」分成 Dom 层（纯函数、立即模式）和 Component 层（保留模式、带事件），这个分层**正好对应**你要学的「core 出事件、tui 维护视图状态」——用它来教概念，比 ncurses 的裸 API 直观得多。

**为什么 HTTP 不用 Boost.Beast**：Beast 是异步的、学习曲线陡。入门阶段用同步 + 独立线程，**先让流程跑通，再谈性能**。

**关于 SSE 的一个省力点**：cpp-httplib 自带 `httplib::sse::SSEClient`，提供 `on_message` / `on_event` 回调、`start_async()` 后台线程模式、自动重连与 `Last-Event-ID` 续传。S2 直接用它，**把精力留给 Agent 逻辑而不是协议解析**。（S4 的取消语义仍需自己接：`ssse.stop()` 可在回调外中断连接。）

---

## 四、阶段划分（已作废，现行版见 `docs/roadmap.md`）

本文初稿分的是 **9 个阶段（S0–S8）**，**现在作废**。v2 把它重排成 12 个阶段、13 个 tag
（S3 拆成 S3a/S3b），理由写在 roadmap §一：**一次只加一个未知数**。

差别最大的两处：

- v1 的 **S1 是"TUI 外壳"** —— v2 把 TUI 挪到 **S9**：先有真实事件，再画界面；
  否则你只能对着假数据调布局，学不到东西还占时间。
- v1 的 **S3 是"Agent 循环与工具"** —— v2 拆成 **S3a（真模型，先不流式）→ S3b（SSE 流式）→ S4（工具循环）**。

| v1（作废） | v2（现行） |
|---|---|
| S0 双进程骨架与协议 | S0 协议设计 + S1 握手 |
| S1 TUI 外壳 | S9 TUI |
| S2 接上 LLM | S3a 真模型 / S3b SSE |
| S3 Agent 循环与工具 | S4 Agent 循环 |
| S4 流式、取消与并发 | S3b（流式）+ S5（取消与合批） |
| S5 上下文管理与压缩 | S6 |
| S6 持久化与多前端 | S7 |
| S7 权限与沙箱 | S8 |
| S8 子智能体、技能、MCP | S10（MCP）+ S11（子智能体 / skills） |

**现行计划的逐阶段详解、DoD、里程碑全在 `docs/roadmap.md`；已完成阶段的设计记录在 `docs/stage-s2..s6.md`。**

## 五、仓库结构（终态）

```
new-agent/
├── CMakeLists.txt
├── docs/
│   ├── protocol.md            # 协议唯一真相
│   ├── stage-s2.md … stage-s*.md  # 每阶段设计说明
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
├── mcp/                       # S10：MCP client
└── tests/
    ├── fake_llm/              # 假 LLM server
    └── ...
```

**分层铁律**：`frontends/*` **只能** include `libs/protocol` 和 `libs/common`。
一旦前端 include 了 `core/provider`，双进程就白做了。可以在 CI 里加一条检查来强制它。

---

## 六、几个必须提前想清楚的坑

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

## 七、这份设计**刻意不做**的事

明确边界，避免范围蔓延：

- ❌ 不做多模态（图片/音频）。
- ❌ 不做 RAG / 向量库（那是另一个主题，可在 S8 后作为独立扩展）。
- ❌ 不做远程/云端会话（Claude Code 的 bridge 那样）。
- ❌ 不做多智能体编排引擎（Workflows 那层）。
- ❌ 不追求性能极致（同步 HTTP + 线程足够）。
- ❌ 不做插件的热加载/ABI 稳定性。

**目标只有一个**：把 Agent 的主干概念**每一条都亲手写一遍**。

---

## 八、建议的学习方式

1. **先读 `docs/protocol.md`**，它是整个系统的枢纽。
2. `git checkout s0`，跑起来，看 `--log-raw` 的原始报文。
3. 逐阶段 `git diff s(N-1)..sN`，**先看 diff 再看代码全文**。
4. 每阶段读完，回答 `docs/stage-sN.md` 里的「替代方案」问题：**如果换成另一种做法会怎样**。
5. 每阶段结束做一次「破坏性实验」：故意去掉循环上限、故意让日志写 stdout，**亲眼看到它怎么坏**。

---

## 九、结语：一句话记住整个架构

> **core 是唯一持有真相（transcript / 上下文 / 权限）的 harness，tui 是它的一个可替换的呈现前端，两者之间流动的永远是结构化事件而非人类文本。**

这句话如果立住了，双进程就立住了；双进程立住了，后面 8 个阶段都只是往 core 里加模块而已。

---

## 十、调研来源

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
- [WHATWG HTML Standard §9.2 —— Server-sent events](https://html.spec.whatwg.org/multipage/server-sent-events.html) —— 事件流解析的权威规范，S3b 算法出处
- [cpp-httplib —— SSE 客户端文档](https://github.com/yhirose/cpp-httplib/blob/master/README-sse.md) —— `on_message` / `start_async` / 自动重连
- [cpp-httplib —— 流式 API 文档](https://github.com/yhirose/cpp-httplib/blob/master/README-stream.md) —— 无 keep-alive、Result 非线程安全
- [cpr —— Server-Sent Events](https://github.com/libcpr/docs/blob/master/sse.md) —— `SetServerSentEventCallback` 的备选方案

### FTXUI 关键 API
- [FTXUI —— app.hpp](https://github.com/ArthurSonzogni/FTXUI/blob/main/include/ftxui/component/app.hpp) —— `Loop` / `Post` / `PostEvent` / `RequestAnimationFrame` / `Exit` / `PrintAbove`
- [FTXUI —— screen_interactive.hpp](https://github.com/ArthurSonzogni/FTXUI/blob/main/include/ftxui/component/screen_interactive.hpp) —— `using ScreenInteractive = App;` 别名

### 参考项目
- [Cursor —— Continually improving our agent harness](https://cursor.com/blog/continually-improving-agent-harness) —— 「harness 独立于模型迭代」的产品视角

---

## 十一、附：配套调研文档

本次调研产出了两份原始报告，内容比本设计文档更细，可作为补充阅读：

| 文件 | 内容 |
|---|---|
| [architecture-brief.md](./architecture-brief.md) | 进程模型、Agent 循环、Harness 组件、IPC 选项的完整调研（含 Codex/Gemini/Claude Code 源码级引用） |
| [tech-stack.md](./tech-stack.md) | C++ 技术选型详报：HTTP/SSE、JSON、TUI、IPC、构建系统、测试策略，含可直接用的 CMakeLists |

---

*文档版本：v1 设计稿（架构 / 协议 / 选型部分仍然有效）｜阶段划分已由 [`docs/roadmap.md`](roadmap.md)（v2）取代*

