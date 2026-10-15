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

**后续阶段将追加（占位，当前未定义，勿提前实现）**：

| 消息 | 引入阶段 | 一句话 |
|---|---|---|
| `demo.chat` + `assistant.delta` | S2 | 假模型跑通事件流 |
| `user.message` / `turn.*` / 真 `assistant.delta` | S3 | 真模型对话 |
| `tool.call` / `tool.result` | S4 | 工具循环 |
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
