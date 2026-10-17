# core 与 cli 的职责清单
> 来源：new-agent-stages-v2.md 第三章。S1 建立，此后每阶段若职责边界变化必须更新本文件。
> 「状态」列：✅ 已实现 ｜ 🔜 本文件已登记、待实现 ｜ ⬜ 未登记

## agent-core（大脑，拥有全部"真相"）

| 职责 | 为什么在 core | 引入阶段 | 状态 |
|---|---|---|---|
| 按协议应答（hello/ping/echo/shutdown） | 协议语义属于 core | S1 | ✅ |
| 假模型（demo.chat）—— S2 的可替换接缝 | 先让事件流跑通；S3 换成真 Provider，**帧形状不变** | S2 | ✅ |
| HTTP 客户端接入（先非流式） | 前端不该知道 HTTP 的存在 | S3a | ✅ |
| SSE 流式解析 | 分片边界、码点切断、重试都发生在 core | S3b | 🔜 |
| 消息历史（真相状态） | 换前端不丢对话 | S3a | ✅ |
| 工具注册表与执行 | 权限判定必须和执行同进程 | S4 | 🔜 |
| Agent 循环（turn 状态机） | 循环是协议语义，不是 UI 语义 | S4 | 🔜 |
| 取消与节流 | 取消要穿透到 HTTP 层 | S5 | 🔜 |
| token 预算与压缩 | 同上 | S6 | 🔜 |
| 会话持久化 | 落盘格式是协议的一部分 | S7 | 🔜 |
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

## 铁律（每阶段 DoD 检查）

1. `agent_common` 绝不链接 HTTP 库或 TUI 库。
2. core 从不向前端发"渲染结果"，只发结构化事件。
3. 前端不认识 HTTP / 工具 / 上下文这些词。
4. **已登记的例外**：`--embedded` 让 `agent-cli` 在链接层面拿到 `agent_core_lib`，
   S3a 起这个库里含 httplib + OpenSSL（Codex 的同款做法：InProcessAppServerClient 也一样）。
   **源码层面的铁律不变**：`src/cli/` 里不得出现 httplib / openssl / ftxui。
   这一条不靠自觉，靠测试：`tests/test_s3a.py::test_layering` 会扫源码，违反就红。

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
