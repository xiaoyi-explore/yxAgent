# core 与 cli 的职责清单
> 来源：new-agent-stages-v2.md 第三章。S1 建立，此后每阶段若职责边界变化必须更新本文件。
> 「状态」列：✅ 已实现 ｜ 🔜 本文件已登记、待实现 ｜ ⬜ 未登记

## agent-core（大脑，拥有全部"真相"）

| 职责 | 为什么在 core | 引入阶段 | 状态 |
|---|---|---|---|
| 按协议应答（hello/ping/echo/shutdown） | 协议语义属于 core | S1 | ✅ |
| 与 LLM 的 HTTP/SSE 通信 | 前端不该知道 HTTP 的存在 | S3 | 🔜 |
| 消息历史（真相状态） | 换前端不丢对话 | S3 | 🔜 |
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
| 把 `assistant.delta` 渲染成打字机效果 | core 从不发"渲染好的字符串" | S2 起 | 🔜 |
| 审批弹窗（只负责"问"和"回传答案"） | 决策在 core | S8 | 🔜 |
| TUI 主循环与事件队列 | FTXUI 的线程模型要求 | S9 | 🔜 |

## 铁律（每阶段 DoD 检查）

1. `agent_common` 绝不链接 HTTP 库或 TUI 库。
2. core 从不向前端发"渲染结果"，只发结构化事件。
3. 前端不认识 HTTP / 工具 / 上下文这些词。

## S1 代码地图

| 文件 | 行数级 | 内容 |
|---|---|---|
| `src/common/protocol.hpp` | ~60 | 构帧工厂 + 解帧 + 版本比较（header-only，零依赖 nlohmann/json 之外的东西） |
| `src/core/main.cpp` | ~70 | 第一帧 event hello → 读循环 → 方法分发 → flush 退出 |
| `src/cli/main.cpp` | ~100 | fork+双管道 spawn → 握手 → echo 主循环 → shutdown 收尾 |