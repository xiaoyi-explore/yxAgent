# core 与 cli 的职责清单
> 来源：roadmap.md 第三章。S1 建立，此后每阶段若职责边界变化必须更新本文件。
> 「状态」列：✅ 已实现 ｜ 🔜 本文件已登记、待实现 ｜ ⬜ 未登记

## agent-core（大脑，拥有全部"真相"）

| 职责 | 为什么在 core | 引入阶段 | 状态 |
|---|---|---|---|
| 按协议应答（hello/ping/echo/shutdown） | 协议语义属于 core | S1 | ✅ |
| 假模型（demo.chat）—— S2 的可替换接缝 | 先让事件流跑通；S3 换成真 Provider，**帧形状不变** | S2 | ✅ |
| HTTP 客户端接入（先非流式） | 前端不该知道 HTTP 的存在 | S3a | ✅ |
| SSE 流式解析（`chat` 的真增量） | 分片边界、码点切断、重试都发生在 core | S3b | ✅ |
| 消息历史（真相状态） | 换前端不丢对话 | S3a | ✅ |
| 工具注册表与执行（read_file / write_file / run_shell） | 权限判定必须和执行同进程 | S4 | ✅ |
| Agent 循环（turn 状态机，ReAct + MAX_STEPS） | 循环是协议语义，不是 UI 语义 | S4 | ✅ |
| 取消与节流 | 取消要穿透到 HTTP 层 | S5 | ✅ |
| token 预算与压缩（`ContextManager`） | 只有 core 知道历史有多长；压缩是"认知"的活，不是 UI 的活 | S6 | ✅ |
| 会话持久化（`SessionStore`） | 落盘格式是协议的一部分；恢复要重建 `history_` | S7 | ✅ |
| 审批判定（allow/ask/deny） | 前端被绕过也不能失守 | S8 | 🔜 |

## agent-cli / agent-tui（脸，只消费事件）

| 职责 | 为什么在前端 | 引入阶段 | 状态 |
|---|---|---|---|
| spawn core + stdio 双管道 | —— | S1 | ✅ |
| 握手（等 event hello → request hello） | 时序的客户端一侧 | S1 | ✅ |
| 收集用户输入 → request | —— | S1 | ✅ |
| 把 `assistant.delta` 渲染成打字机效果（逐帧 flush） | core 从不发"渲染好的字符串" | S2 | ✅ |
| 审批弹窗（只负责"问"和"回传答案"） | 决策在 core | S8 | 🔜 |
| TUI 主循环与事件队列 | FTXUI 的线程模型要求 | S9 | 🔜 |

## agent-run（headless 前端，S7）

| 职责 | 为什么在 agent-run | 引入阶段 | 状态 |
|---|---|---|---|
| spawn core + 握手 + `chat` | 和 cli 完全同一条协议路径 | S7 | ✅ |
| stdout 最终答复 / stderr 进度 | 机器消费要干净输出 | S7 | ✅ |
| `--session` / `--resume` | 一次性任务也要能接上历史 | S7 | ✅ |
| **不 include core、不链 `agent_core_lib`** | 证明"同一 core，两个前端"没有偷偷 `--embedded` | S7 | ✅ |

## 铁律（每阶段 DoD 检查）

1. `agent_common` 绝不链接 HTTP 库或 TUI 库。
2. core 从不向前端发"渲染结果"，只发结构化事件。
3. 前端不认识 HTTP / 工具 / 上下文这些词。
4. **已登记的例外**：`--embedded` 让 `agent-cli` 在链接层面拿到 `agent_core_lib`，
   S3a 起这个库里含 httplib + OpenSSL（Codex 的同款做法：InProcessAppServerClient 也一样）。
   **源码层面的铁律不变**：`src/cli/` 里不得出现 httplib / openssl / ftxui。
   这一条不靠自觉，靠测试：`tests/test_s3a.py::test_layering` 会扫源码，违反就红。
5. **HTTP 只准出现在 `src/core/openai_compat.cpp` 一处。**
   `src/core/sse.*` 是纯字节解析，不认识 HTTP（连带不知道 OpenAI）；
   它甚至**不链接** httplib/OpenSSL——`sse_parser_test` 这个构建目标就是证据，
   `tests/test_s3b.py::test_layering` 再扫一遍源码。
6. **工具只在 core 里执行。** 前端只看得见 `tool.call` / `tool.result` 两个事件，
   永远不碰文件系统、不执行命令；`src/core/tools.cpp` 自己也不认识 HTTP。
   靠测试守：`tests/test_s4.py::test_layering`。
7. **`agent-run` 只链接 `agent_common`，不碰 `agent_core_lib` / httplib / OpenSSL。**
   它是第二个前端，不是 core 的第二个入口；靠 `tests/test_s7.py` 的 grep 守。

## S1 代码地图

| 文件 | 行数级 | 内容 |
|---|---|---|
| `src/common/protocol.hpp` | ~60 | 构帧工厂 + 解帧 + 版本比较（header-only，零依赖 nlohmann/json 之外的东西） |
| `src/core/main.cpp` | ~70 | 第一帧 event hello → 读循环 → 方法分发 → flush 退出 |
| `src/cli/main.cpp` | ~100 | fork+双管道 spawn → 握手 → echo 主循环 → shutdown 收尾 |

## S3a 代码地图

| 文件 | 行数级 | 内容 |
|---|---|---|
| `src/core/provider.hpp` | ~45 | `Message` / `ChatResult` / `Provider` 接口 / `ProviderError`（无 HTTP 依赖） |
| `src/core/openai_compat.hpp/.cpp` | ~30 / ~120 | OpenAI 兼容客户端：拆 base_url、POST、状态码与 JSON 校验 |
| `src/core/core_engine.cpp` | +40 | `chat` 方法：历史维护、失败回滚、统一 `PROVIDER_ERROR` |
| `third_party/httplib.h` | vendored | cpp-httplib v0.18.3 单头文件（含 OpenSSL 支持） |
| `tests/fake_llm.py` | ~90 | 假 LLM server：200 / 401 / 500 / 非 JSON / 慢 / failonce 六种路径 |
| `tests/test_s3a.py` | ~230 | 成功、多轮、错误映射、超时、失败回滚、cli e2e、分层铁律 |
## S3b 代码地图

| 文件 | 行数级 | 内容 |
|---|---|---|
| `src/core/sse.hpp/.cpp` | ~80 / ~100 | SSE 增量解析器：按行缓冲 → 空行派发 → 多行 data 拼接 → 注释行忽略；`flush()` 收拾没有空行收尾的最后一段。**不 include 任何网络库** |
| `src/core/provider.hpp` | +15 | `DeltaSink`；`chat_stream()`（默认实现退化成整段一次吐）；`supports_stream()` |
| `src/core/openai_compat.hpp/.cpp` | ~55 / ~230 | `stream` 开关与 `max_attempts`；`chat_stream()`：拼 `Request`（httplib 的 Post 没有流式重载）→ 边收边喂解析器 → 状态码/断连/非 SSE 回退；只重试传输层失败 |
| `src/core/core_engine.cpp` | +10 | `chat` 里把增量 emit 成 `assistant.delta`；响应形状不变 |
| `src/core/main.cpp` + `src/cli/main.cpp` | +5 / +12 | `--stream/--no-stream` 透传；cli 不再在结算时重播整段 |
| `tests/sse_parser_test.cpp` | ~150 | **穷举切点**单元测试（1 字节一喂 … 一次喂完，含 CRLF 变体）；只链接 `sse.cpp` |
| `tests/fake_llm_sse.py` | ~180 | 假 SSE server：自然切块 / 切在 JSON 中间 / 切在码点中间 / 注释行 / 慢流 / 断连 / 401·429·500 / 掐断连接 / 无 usage / 无 `[DONE]` |
| `tests/fixtures/sse_*.sse` | 4 个文件 | 可读的字节级 fixture（`{{MSGS}}`/`{{ECHO}}` 占位符由假 server 渲染） |
| `tests/test_s3b.py` | ~330 | 74 项检查：六条 fixture + 断连 + 重试 + 两条回退路径 + cli e2e + 铁律 |
## S6 代码地图

| 文件 | 行数级 | 内容 |
|---|---|---|
| `src/core/context.hpp` / `.cpp` | ~60 / ~230 | `ContextManager`：估算 → 三级压缩（裁剪 / 摘要 / 兜底）。只认 `vector<Message>`，不认识协议/HTTP |
| `src/core/core_engine.cpp` | +30 | 每个 step 进模型前压一次；发 `context.compacted`；`history_mark` 跟着往下挪 |
| `tests/context_test.cpp` | ~250 | 纯单元测试（只链 `context.cpp`）：配对、豁免、兜底、不许变大，共 28 项 |
| `tests/test_s6.py` | ~200 | 端到端：事件、`/__last` 里的压缩后请求、关压缩、cli 不再编造 tokens |

## S7 代码地图

| 文件 | 行数级 | 内容 |
|---|---|---|
| `src/core/session.hpp` / `.cpp` | ~90 / ~430 | `SessionStore`：NDJSON 追加写、header/checkpoint/commit/abort、flock 单写者、`last` 指针、恢复时按记录重建 `history_` |
| `src/core/core_engine.cpp` | +120 | attach/load 会话、`session.load`、`log()` 顺带落帧、轮末 commit / 失败 abort、压缩 checkpoint |
| `src/core/main.cpp` | +35 | `--session` / `--resume` / `--session-dir` + 错误退出码 |
| `src/cli/main.cpp` | +30 | 透传会话开关、握手后显示 `[session] resumed/new` |
| `src/run/main.cpp` | ~300 | `agent-run`：最小协议客户端 + 最终答复输出；只链 `agent_common` |
| `tests/test_s7.py` | ~520 | 10 组：格式、历史、seq/turn、压缩摘要、工具历史、失败轮、崩溃、session.load、cli、agent-run |

## S4 代码地图

| 文件 | 行数级 | 内容 |
|---|---|---|
| `src/core/tools.hpp/.cpp` | ~90 / ~340 | `Tool` 接口 / `ToolRegistry`（未知工具返回失败而不是抛异常）/ 三个内置工具：`read_file`、`write_file`、`run_shell`（自带超时看门狗，stdout+stderr 合并） |
| `src/core/provider.hpp` | +25 | `ToolCall` / `ToolSpec` / `ChatRequest`；`chat(req)` / `chat_stream(req, sink)`；`Message` 长出 `tool_calls` / `tool_call_id` / `name` |
| `src/core/openai_compat.cpp` | +60 | `to_wire()`（唯一出现 OpenAI 字段名的地方）、tools 声明；**流式 tool_calls 按 index 累积** |
| `src/core/core_engine.cpp` | +95 | ReAct 循环、`MAX_STEPS`、整轮回滚、`tool.call` / `tool.result` 事件、delta 带 `step`、**系统提示词进 `history_[0]`** |
| `src/core/main.cpp` + `src/cli/main.cpp` | +10 / +45 | `--tools/--workdir/--max-steps` 透传；cli 渲染工具行、按 `step` 决定结算时要不要重播全文 |
| `tests/fake_llm_tools.py` | ~250 | 假 server：**看请求说话**（最后一条是 tool 就收尾，否则要工具）。分片参数、多工具、坏 JSON、超时、永不停机等十种剧本 |
| `tests/test_s4.py` | ~330 | 45 项检查：真执行三个工具 + 分片累积 + 失败回灌 + MAX_STEPS + 非流式 + tools 声明开/关 + cli e2e |
