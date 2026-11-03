# yxAgent 协议 —— core ⇄ front 通信协议（v1.0）

> 本文档是 yxAgent 的**唯一协议真相**。S0 只定义骨架：分帧、信封、三种消息、握手与关闭。
> LLM、工具、会话相关消息**故意不在这里**——它们分别由 S2/S3/S7 等阶段追加。
> 每个后续阶段只允许**追加**新消息，不允许修改已定义消息的语义（见 §7 演进规则）。

---

## 1. 分帧（Framing）

传输层是 **stdio 管道**（spawn 子进程后接管它的 stdin/stdout）。分帧规则：

1. **一帧 = 一行**，UTF-8 编码，以 LF（\n）结尾。
2. 帧内是**一个完整 JSON 对象**，内部不允许出现裸换行——字符串里的换行必须转义成 `\\n`。这就是 NDJSON。
3. 写端写完一行必须**立即 flush**（C++ 里注意 `std::endl` 或 `flush()`，`\\n` 不自动刷新）。
4. 单帧上限 **1 MiB**（含行尾）。超限视为协议错误：读端丢弃该行、能回则回一条错误响应、记日志。
5. 读端按行累积字节；一次 read 拿到半行或三行都正常，**必须缓冲到看到 LF 再解析**。

为什么选 NDJSON 而不是长度前缀或 HTTP：人眼可直接读（`--log-raw` 的前提）、行数即帧数（日志对账）、`jq` 可逐行验证。长度前缀留给未来 S7 的 unix socket 复用（同规则，只换传输层）。

---

## 2. 信封（Envelope）

每一帧都是以下三种信封之一，**没有例外**：

### 2.1 request —— cli → core，必须收到 response

```json
{"type":"request","id":"r1","method":"hello","params":{"protocol_version":"1.0","client_version":"0.0.1"}}
```

| 字段 | 类型 | 规则 |
|---|---|---|
| `type` | string | 恒为 `"request"` |
| `id` | string | **cli 生成**，连接内唯一。推荐 `"r"+十进制递增`。core 不解释，只回填 |
| `method` | string | 方法名，snake_case |
| `params` | object | 参数，由各 method 定义；可为空对象 |

### 2.2 response —— core → cli，对应某个 request

```json
{"type":"response","id":"r1","ok":true,"result":{"protocol_version":"1.0","core_version":"0.0.1"}}
```

```json
{"type":"response","id":"r9","ok":false,"error":{"code":"UNKNOWN_METHOD","message":"no such method: foo"}}
```

| 字段 | 类型 | 规则 |
|---|---|---|
| `type` | string | 恒为 `"response"` |
| `id` | string\|null | 回填对应 request 的 `id`；**无法确定 id 时用 `null`**（如收到垃圾帧） |
| `ok` | bool | true 时必有 `result`；false 时必有 `error` |
| `error.code` | string | 见 §5 错误码表 |
| `error.message` | string | 人可读，调试用 |

> response 的到达顺序**不必**与 request 顺序一致，靠 `id` 配对。S0 只做一问一答，但协议从第一天就允许并发请求——S5 的取消要靠它。

### 2.3 event —— core → cli，单向通知，不回任何东西

```json
{"type":"event","seq":1,"name":"hello","payload":{"protocol_version":"1.0","core_version":"0.0.1","pid":4312}}
```

| 字段 | 类型 | 规则 |
|---|---|---|
| `type` | string | 恒为 `"event"` |
| `seq` | int | **core 生成**，连接内从 1 开始、严格递增、无空洞 |
| `name` | string | 事件名，snake_case |
| `payload` | object | 由各事件定义 |

`seq` 的三个用途：日志对账（肉眼找丢帧）、S7 的会话续传游标、丢帧即 bug（收到 `seq=5` 而上一帧是 `seq=3`，说明 core 有问题，直接报错而不是跳过）。

---

## 3. 消息索引

**S0 定义的完整清单**（此后每阶段在此表追加行）：

| 消息 | 方向 | 信封种类 | 引入阶段 | 说明 |
|---|---|---|---|---|
| `hello`（事件） | core→cli | event | S0 | core 就绪声明，永远是连接的第一帧 |
| `hello`（方法） | cli→core | request/response | S0 | 版本协商，成功响应即 `welcome` 语义 |
| `ping`（方法） | cli→core | request/response | S0 | 存活探测与延迟测量 |
| `shutdown`（方法） | cli→core | request/response | S0 | 协商关闭 |
| `echo`（方法） | cli→core | request/response | S1 | 开发期回显；S1 的主力调试消息，S2 起被 demo.chat 取代 |
| `demo.chat`（方法） | cli→core | request/response | S2 | 假模型对话：先吐一串 `assistant.delta`，最后才回响应 |
| `assistant.delta`（事件） | core→cli | event | S2 | 流式增量文本；一个 delta 恰好一个 UTF-8 码点 |

**后续阶段将追加（占位，当前未定义，勿提前实现）**：

| 消息 | 引入阶段 | 一句话 |
|---|---|---|
| `user.message` / `turn.*` / 真 `assistant.delta` | S3b | 流式对话（S3a 的 `chat` 是非流式前身） |
| `tool.call` / `tool.result` | S4 | 工具循环 |
| `turn.cancel`（方法） | S5 | 打断正在跑的一轮；本轮以 `stop_reason=cancelled` 正常结算 |
| `turn.cancel` | S5 | 取消 |
| `context.compacted` | S6 | 压缩通知 |
| `session.*` | S7 | 持久化与续传 |
| `approval.requested` / `approval.respond` | S8 | 审批往返 |

---

## 4. 生命周期与握手

### 4.1 握手时序（完整真实报文）

```
cli                                                core
 |   (spawn 子进程，接管 stdio 管道)                   |
 |                                                  | core 启动完成
 | <──── {"type":"event","seq":1,"name":"hello",     | ← 第一帧永远是它
 |          "payload":{"protocol_version":"1.0",     |
 |          "core_version":"0.0.1","pid":4312}}      |
 |                                                  |
 | ────> {"type":"request","id":"r1","method":"hello",      |
 |          "params":{"protocol_version":"1.0",      |
 |          "client_version":"0.0.1"}}               |
 |                                                  | 版本检查
 | <──── {"type":"response","id":"r1","ok":true,     |
 |          "result":{"protocol_version":"1.0",      |
 |          "core_version":"0.0.1"}}                 |
 |                                                  |
 |        ══ 会话建立，进入工作状态 ══                 |
 |                                                  |
 | ────> {"type":"request","id":"r2","method":"ping",       |
 |          "params":{"client_ts":1728970200}}       |
 | <──── {"type":"response","id":"r2","ok":true,     |
 |          "result":{"client_ts":1728970200,        |
 |          "core_ts":1728970200}}                   |
 |                                                  |
 | ────> {"type":"request","id":"r3","method":"shutdown",   |
 |          "params":{}}                             |
 | <──── {"type":"response","id":"r3","ok":true,     |
 |          "result":{"will_exit":true}}             |
 |                                                  | core flush 全部缓冲
 |        ══ core 退出，退出码 0 ══                    |
```

### 4.2 各消息语义

**event `hello`（core→cli，就绪声明）**

- 必须是连接的**第一帧**。cli 在收到它之前发出的任何帧，core 仍须正常处理（管道有缓冲），但 cli 端 UI 应显示"启动中"。
- `payload.protocol_version`：core 支持的协议版本，当前 `"1.0"`。

**request `hello`（cli→core，版本协商）**

- 版本比较规则：**major 相同即兼容**（`1.x` 互通，按双方较小的 minor 行事）；major 不同 → 走错误路径。
- 兼容：response `ok:true`，`result` 回 core 的版本信息。**这个响应就是 "welcome"**——不设单独的 welcome 消息，少一个信封就少一种状态。
- 不兼容：response `ok:false`，`error.code = "PROTOCOL_VERSION_MISMATCH"`，随后 core 主动退出（退出码 2）。

**request `ping`**

- `params.client_ts`：cli 的时间戳（秒，可省略）。core 原样回填并附上自己的时间。用途：测 RTT、写进日志。

**request `shutdown`**

- core 收到后：停止接受新请求 → 处理完进行中的工作（S0 没有）→ flush → 回 response → `exit(0)`。
- cli 收到 response 后关闭管道，等待子进程退出，**超时 5 秒强制 kill 并记日志**。

### 4.3 关闭与死亡的三种方式

| 场景 | 现象 | 处理 |
|---|---|---|
| 协商关闭 | cli 发 shutdown，走完 4.1 时序 | 双方干净退出，退出码 0 |
| cli 死了 | core 从 stdin 读到 EOF | core flush 后 `exit(0)`，不写半截帧 |
| core 死了 | cli 从 stdout 读到 EOF | cli 视为异常，向用户报错（S0 只需打印到 stderr） |

> 铁律：**任何一端退出前必须 flush**。NDJSON 没有帧边界标记，半行 = 永远解不开的帧。

---

## 5. 错误码表

| code | 含义 | response 的 id |
|---|---|---|
| `PARSE_ERROR` | 帧不是合法 JSON | `null` |
| `INVALID_ENVELOPE` | JSON 合法但信封字段缺失/类型错 | `null` 或能提取到的 id |
| `UNKNOWN_METHOD` | method 不存在 | 回填 |
| `INVALID_PARAMS` | params 校验失败 | 回填 |
| `PROTOCOL_VERSION_MISMATCH` | major 版本不兼容 | 回填；core 随后退出 |
| `INTERNAL_ERROR` | core 内部异常 | 回填 |

两条硬规则：

1. **请求不能悬空**：只要能从帧里提取出 `id`，就必须有 response——哪怕是错误响应。
2. **错误要可行动**：`error.message` 必须写到"人看了知道下一步做什么"，不允许只写 `"error"`。

---

## 6. 兼容性：未知即忽略

前向兼容是双进程协议的命根子（cli 和 core 可能不同步升级）。规则从 S0 定死：

| 情况 | 处理 |
|---|---|
| 未知 `type` | 丢弃该帧，记日志，**不响应** |
| 未知 event `name` | 丢弃，记日志 |
| 信封里的未知字段 | 忽略 |
| `params`/`payload` 里的未知字段 | 忽略 |
| 未知 `method` | **必须**回 `UNKNOWN_METHOD`（请求不能悬空） |

---

## 7. 演进规则（本文档是活文档）

1. 只**追加**消息，不修改已定义消息的语义；确需修改 = major 版本升级 + 新文档。
2. 每个新消息必须同时给出：一行真实报文示例 + 字段表 + 引入它的阶段 tag。
3. 消息命名：事件用名词或名词短语（`assistant.delta`）；方法用动词短语（`shutdown`）。
4. 字段命名一律 snake_case；时间一律 Unix 秒；大小一律字节。
5. 本文档改动的每个 commit 必须能在对应的 git tag（s0…s11）里通过该阶段的测试。

---

## 8. S0 的纸上验收（0 行代码怎么验收？）

S0 没有代码，但可以做三件事，做完才算 S0 完成：

1. **报文自检**：把 §4.1 的 transcript 逐帧粘进 `jq`（或任意 JSON 校验器），每行必须解析成功且 `type` 字段齐全。
2. **走查测试**：对照 §6 逐条问自己——"如果 core 收到 `{"type":"request","id":"r1","method":"foo"}`，文档规定我做什么？"（答：回 UNKNOWN_METHOD。）答不上任何一条就回去重读。
3. **留档**：把 §4.1 的报文原样存为 `tests/fixtures/handshake.ndjson`——它就是 S1 握手测试的 golden fixture。**S0 的产出物是 2 个文件**：`docs/protocol.md` 和这个 fixture。

**S0 完成标志**：`git tag s0`。

---

---

## 9. 阶段追加记录

### S1 追加：`echo` 方法

| 项 | 内容 |
|---|---|
| 报文 | `{"type":"request","id":"r4","method":"echo","params":{"line":"hi"}}` |
| 响应 | `{"type":"response","id":"r4","ok":true,"result":{"line":"hi"}}` |
| 语义 | core 把 `params` 原样放进 `result` 返回。它证明管道双向通了，是 cli 的一切后续能力的基础 |
| 去向 | 调试消息，S2 接入 demo.chat 后逐步退役 |

配套 fixture：`tests/fixtures/echo.ndjson`。

### S2 追加：`demo.chat` 方法与 `assistant.delta` 事件

S0 说"一个请求 ↔ 一个响应"，S2 首次让中间长出**事件流**：

```
cli ──request demo.chat {text} ──────────────────────► core
cli ◄─event assistant.delta {turn_id, text} ───────── core   seq=N
cli ◄─event assistant.delta {turn_id, text} ───────── core   seq=N+1
cli ◄─event assistant.delta ... ───────────────────── core
cli ◄─response {turn_id, text, stop_reason} ───────── core   ← 流结束后才结算
```

| 项 | 内容 |
|---|---|
| 请求 | `{"type":"request","id":"r5","method":"demo.chat","params":{"text":"你好"}}` |
| 事件 | `{"type":"event","seq":2,"name":"assistant.delta","payload":{"turn_id":"t1","text":"收"}}` |
| 响应 | `{"type":"response","id":"r5","ok":true,"result":{"turn_id":"t1","text":"收到第 1 句：「你好」—— ...","stop_reason":"stop"}}` |
| **不变量 1** | response 一定是本轮**最后一帧**（它宣告本轮结束） |
| **不变量 2** | `result.text` 恒等于本轮所有 `assistant.delta.text` 的**顺序拼接** |
| **不变量 3** | 一个 delta 的 `text` 恰好一个 UTF-8 码点，绝不含半个汉字 |
| 语义 | S2 的假模型是确定性的；S3 换成真 Provider，但**帧的形状不变** |
| fixture | `tests/fixtures/demo_chat.ndjson`（golden：可逐帧复现） |

为什么响应放最后，而不是先回一个 ack 再流式推送？见 `docs/stage-s2.md` 的决策记录——
一句话：真实 LLM 也是"流完才知道结果"，先这样设计，S3 就不用再改协议形状。

### S3a 追加：`chat` 方法与 Provider 抽象

| 项 | 内容 |
|---|---|
| 请求 | `{"type":"request","id":"r2","method":"chat","params":{"text":"你好"}}` |
| 响应 | `{"type":"response","id":"r2","ok":true,"result":{"turn_id":"t1","text":"…","stop_reason":"stop","usage":{"prompt_tokens":9,"completion_tokens":12,"total_tokens":21}}}` |
| 语义 | core 调 OpenAI 兼容端点的 `POST /chat/completions`（`stream:false`），拿**整段**回复 |
| 历史 | `messages` 数组由 **core** 维护（真相在 core）；失败的一轮不写入历史 |
| 无事件 | 本阶段不产生任何 event（`assistant.delta` 是 S3b） |
| 错误 | `NO_PROVIDER`：启动时没配模型端点；`PROVIDER_ERROR`：HTTP 错误 / 非 JSON / 超时 / 连接失败，message 必须可行动 |

S3b 已经把这里的 `stream:false` 改成了 `true`，并让每个增量变成 `assistant.delta` 事件——
**响应的形状没变**，仍然在本轮结束时结算（这就是 S2 定"响应迟到"的回报）。

### S3b 追加：`chat` 变成流式（SSE）

`stream:false` → `stream:true`，`chat` 长出了 `assistant.delta`，**请求与响应的字段一个字没改**。

```
cli ──request chat {text} ───────────────────────────────► core
cli ◄─event assistant.delta {turn_id, text} ───────────── core   seq=N    ← 每收到一段 SSE 就发
cli ◄─event assistant.delta {turn_id, text} ───────────── core   seq=N+1
cli ◄─response {turn_id, text, stop_reason, usage} ────── core            ← 流结束后才结算
```

| 项 | 内容 |
|---|---|
| 请求 | `{"type":"request","id":"r2","method":"chat","params":{"text":"你好"}}`（与 S3a 相同） |
| 事件 | `{"type":"event","seq":2,"name":"assistant.delta","payload":{"turn_id":"t1","text":"我是"}}` |
| 响应 | 与 S3a 相同：`{"type":"response","id":"r2","ok":true,"result":{"turn_id":"t1","text":"…","stop_reason":"stop","usage":{…}}}` |
| **不变量 1** | response 仍是本轮**最后一帧**（S2 定的，S3b 不改） |
| **不变量 2** | `result.text` 恒等于本轮所有 `assistant.delta.text` 的顺序拼接 |
| **不变量 3** | 每个 delta 的 `text` 是**完整的 UTF-8 字符串**（绝不含半个码点）。它与 S2 那条的差别：`demo.chat` 是"一个 delta 恰好一个码点"，`chat` 的 delta 是模型给的一段（可能几个字），前端只需按字符串追加 |
| 上游 | core 对 OpenAI 兼容端点发 `stream:true` + `stream_options.include_usage`，按 SSE 增量解析 |
| 回退 | 对端无视 `stream:true`、直接回整段 JSON 时，core 自动按非流式解析：**整段文本作为一个 delta**，行为等同 S3a |
| 开关 | `agent-core --no-stream` / `agent-cli --no-stream` → 完全回到 S3a 的整段返回 |
| 错误 | 仍只用 `PROVIDER_ERROR`（状态码 / 非 JSON / 超时 / 连接失败 / 流中途断开），message 必须可行动 |
| 重试 | 只重试**传输层**失败（请求没落地），且仅当本轮**还没吐过任何 delta**；吐过字不许重试（会重复输出） |
| fixture | `tests/fixtures/sse_basic.sse` / `sse_comment.sse` / `sse_nousage.sse` / `sse_nodone.sse`（由 `tests/fake_llm_sse.py` 回放） |

**SSE 与我们的 NDJSON 对照**（同一个"分帧"问题，两种答案）：

| | 我们的行协议 | 上游 SSE |
|---|---|---|
| 一帧的边界 | 换行 `\n` | **空行** |
| 载荷前缀 | 无 | `data: `（多行 data 用换行拼接） |
| 心跳 | 无 | `: ` 开头的注释行 |
| 结束标记 | 对端关闭 / `shutdown` | `data: [DONE]`（之后连接可能还开着） |
| 拆包风险 | 半行 | 半行 / 半个 `\r\n` / 半个 UTF-8 码点 |

设计记录（为什么这么拆、替代方案、踩过的坑）见 `docs/stage-s3b.md`。
### S4 追加：`tool.call` / `tool.result` 与 agent 循环

`chat` 不再只是"问一句答一句"：core 装上 `--tools` 之后，一个 turn 里会**多次调用模型**（ReAct 循环）。
协议只多了两个事件和一个字段——**响应的形状不变**，仍然在本轮结束时结算。

```
cli ──request chat {text} ────────────────────────────────────────────► core
cli ◄─event assistant.delta {turn_id, step:1, text} ───────────────── core   ← 第 1 步的增量
cli ◄─event tool.call       {turn_id, step:1, call_id, name, arguments} ─ core
cli ◄─event tool.result     {turn_id, step:1, call_id, name, ok, output, error, duration_ms} ─ core
cli ◄─event assistant.delta {turn_id, step:2, text} ───────────────── core   ← 第 2 步的增量
cli ◄─response {turn_id, text, stop_reason, usage, steps, tool_calls} ─ core
```

| 项 | 内容 |
|---|---|
| 请求 | 与 S3a 相同：`{"type":"request","id":"r3","method":"chat","params":{"text":"统计 a.txt 的行数"}}` |
| `tool.call` | `{"type":"event","seq":3,"name":"tool.call","payload":{"turn_id":"t1","step":1,"call_id":"call_1","name":"read_file","arguments":{"path":"a.txt"}}}` |
| `tool.result` | `{"type":"event","seq":4,"name":"tool.result","payload":{"turn_id":"t1","step":1,"call_id":"call_1","name":"read_file","ok":true,"output":"…文件内容…","error":"","duration_ms":0}}` |
| 增量新字段 | `assistant.delta` 多了 `step`（从 1 开始）。**一个 turn 可能有多个 step**，前端靠它知道"这句话是哪一轮说的" |
| 响应新字段 | `steps`（本 turn 调了几次模型）、`tool_calls`（一共调了几次工具）；其余字段与 S3b 相同 |
| **不变量 1** | response 仍是本轮**最后一帧**（S2 定的，S4 不改） |
| **不变量 2（修订）** | `result.text` 恒等于**最后一个 step** 的 `assistant.delta.text` 顺序拼接。中间 step 的文字是模型的"自言自语"（它可能先说"我看看"再调工具），同样会以 delta 发出——所以不能再写成"所有 delta 的拼接" |
| **不变量 3** | 每个 delta 仍是完整的 UTF-8 字符串 |
| **不变量 4** | `tool.call` 与 `tool.result` 的 `call_id` 一一配对，且同一个 id 的 call 一定在 result 之前 |
| **不变量 5** | 同一个 turn 内 `seq` 严格递增且无空洞（工具事件也占 seq） |
| 循环上限 | `--max-steps N`（默认 8）。超了 → 错误响应 `MAX_STEPS`，本 turn 的整段历史回滚 |
| 工具失败 | **不是 turn 失败**：`ok:false` + `error` 会以 `role=tool` 的消息回灌给模型（`ERROR: …`），让它自己纠正 |
| 坏参数 | 模型的 `arguments` 不是合法 JSON 对象时：`tool.call` 里带 `arguments_raw`（原文），工具**不执行**，`tool.result.ok=false` |
| 权限 | S4 没有审批：工具直接执行。`--tools` **默认关闭**就是这个阶段的阀门（审批在 S8 才做） |
| 系统提示词 | **不在这条协议里**：core 自己把它放在 `history[0]`（`--system` 或内置 agent 提示词），每次请求自动重发。前端看不见、也不需要知道——它只消费事件 |

设计记录（为什么循环在 core、为什么默认关工具、踩过的坑）见 `docs/stage-s4.md`。
### S5 追加：`turn.cancel` 与增量合批

取消**不是**新事件，而是新**方法**：前端在等响应期间发它，core 立刻停手，
并且**照样回一个正常的 response**（这是 S2 定下的"每个请求恰好一个响应"）。

```
cli ──request chat {text} ─────────────────────────────────────────► core
cli ◄─event assistant.delta {turn_id, step:1, text} ─────────────── core
cli ──request turn.cancel {id:"c1"} ───────────────────────────────► core    ← 用户按了 Ctrl-C
cli ◄─response {id:"c1", ok:true, result:{cancelled:true, turn:1}} ─ core    ← 取消的 ack
cli ◄─response {id:"r1", result:{turn_id, text:"…半句…", stop_reason:"cancelled", …}} ─ core
```

| 项 | 内容 |
|---|---|
| 请求 | `{"type":"request","id":"c1","method":"turn.cancel","params":{}}` |
| 响应 | `{"type":"response","id":"c1","ok":true,"result":{"cancelled":true,"turn":1}}`；没在跑时 `{"cancelled":false}` 且不带 `turn` |
| 本轮的结算 | 仍是正常 response，`stop_reason` 变成 `"cancelled"`；`text` 是**已经产出**的那部分（不是空的，除非一个字都没来得及） |
| **不变量 1** | response 仍是本轮最后一帧 |
| **不变量 2（不变）** | `result.text` 仍等于本轮已发出 delta 的顺序拼接——**取消失败在这条上是最容易犯的错** |
| 取消的穿透 | 一路走到 `content_receiver` 返回 false，HTTP 传输被当场掐断（不是"收完再丢"） |
| 历史 | 取消**不回滚**：user 消息与已产出的半句都留在历史里（与"失败整轮回滚"相反，理由见 `stage-s5.md`） |
| 非流式 | `--no-stream` 下取消只能在模型调用返回后生效（POST 掐不断），响应仍是 `cancelled` |
| `--embedded` | **不支持取消**：同步回调、单线程，没有"等响应时还能读输入"的窗口 |
| 增量合批 | `assistant.delta` 默认按 ~30ms 合批（`--delta-batch-ms N`，0 = 一增量一帧）。**只改帧密度，不改拼接** |
| 假模型 | `demo.chat` **不合批**：S2 的验收基线是"一个 delta 恰好一个码点" |
| 空闲取消 | 不报错（用户按 Ctrl-C 的时机永远可能落在两轮之间） |

设计记录（为什么用 poll 而不是线程、为什么取消不回滚、踩过的三个坑）见 `docs/stage-s5.md`。
### S6 追加：`context.compacted` 事件

压缩发生在 **core 内部**，前端**感觉不到**：它收到的 `assistant.delta` 与不压缩时一模一样。
唯一可见的是"事儿发生了"这一条通知。

```
cli ──request chat {text} ─────────────────────────────────────────► core
cli ◄─event context.compacted {before_tokens, after_tokens, …} ──── core   ← 只在真压过时出现
cli ◄─event assistant.delta {turn_id, step, text} ───────────────── core
cli ◄─response {turn_id, text, stop_reason, usage, …} ───────────── core
```

| 项 | 内容 |
|---|---|
| 事件 | `{"type":"event","seq":7,"name":"context.compacted","payload":{"turn_id":"t3","step":1,"budget":32000,"before_tokens":18400,"after_tokens":9100,"dropped_messages":12,"summarized_turns":3,"truncated_tool_results":2}}` |
| 位置 | **在任何 `assistant.delta` 之前**——它发生在"模型还没被调用"的时候 |
| **不变量 1** | response 仍是本轮最后一帧 |
| **不变量 2** | delta 的拼接仍等于 `result.text`——压缩不改"这一轮说了什么" |
| 前端该做什么 | 可以什么都不做；cli 只是往 **stderr** 打一行"已压缩 ~N -> ~M tokens"（不污染 stdout） |
| 谁的真相 | 上下文**只在 core**：前端从来不知道历史有多长，也不该知道 |
| 开关 | `--max-context-tokens N`（估算上限，0 = 关掉）、`--keep-recent-turns N`（默认 3 轮原样保留） |
| 估算 vs 账本 | 预算是**估算**（字节/4，中文偏低）；`usage` 才是真账本。对端不给 usage 时**不许编 0** |
| 历史形态 | 压缩后 `messages` 里会出现一条 `role:"user"`、以 `[压缩]` 开头的摘要消息（在第 0 条 system 之后） |

设计记录（三级压缩、轮级豁免、为什么"不许变大"、踩过的四个坑）见 `docs/stage-s6.md`。