# yxAgent

一个**能通读、能改**的教学级 Agent：C++17 双进程。
`agent-core` 是大脑（模型调用 / 历史 / 工具 / 预算 / 审批），`agent-cli` 是脸（只消费事件），
两者用 **NDJSON over stdio** 说话。

> **现在的状态：S6 完成**（tag `s6`），9 套测试全绿。
> 下一步做什么、卡在哪、怎么验证 → **[docs/HANDOFF.md](docs/HANDOFF.md)** ← 新会话先读这个。

---

## 30 秒跑起来

```bash
cmake -B build -S . && cmake --build build -j8
cd build && ctest          # 9 套：协议 / 事件流 / 真模型 / SSE / 工具 / 取消 / 压缩

# 假模型（不联网、不要 key）
printf '你好\n' | ./agent-cli --demo

# 真模型（key 只从环境变量来，别进命令行）
export DEEPSEEK_API_KEY=sk-...
printf '你好\n' | ./agent-cli --base-url https://api.deepseek.com --model deepseek-chat

# 真模型 + 工具（工具默认是关的，必须显式给）
printf '读一下 a.txt，统计行数\n' | ./agent-cli --tools all --workdir /tmp/x \
       --base-url https://api.deepseek.com --model deepseek-chat
```

---

## 文档地图（每种问题只查一处）

| 我想知道 | 看 |
|---|---|
| **现在进行到哪、下一步做什么、有哪些坑** | **[docs/HANDOFF.md](docs/HANDOFF.md)** |
| 线上到底发什么帧（协议契约 + 不变量） | [docs/protocol.md](docs/protocol.md) |
| 谁该拥有什么职责、铁律、逐阶段代码地图 | [docs/responsibilities.md](docs/responsibilities.md) |
| 某个阶段**为什么**这么设计、替代方案、踩的坑 | `docs/stage-s2.md` … `docs/stage-s6.md` |
| 阶段计划与每阶段 DoD（S0–S11，S3 拆成 a/b） | [docs/roadmap.md](docs/roadmap.md) |
| 为什么选这套 C++ 技术栈 | [docs/tech-stack.md](docs/tech-stack.md) |
| 业界怎么做（Codex / Gemini / Claude Code 源码级调研） | [docs/architecture-brief.md](docs/architecture-brief.md) |
| 设计手册（v1 设计稿）：架构 / 协议 / 选型"为什么这么定" | [docs/manual.md](docs/manual.md) —— **阶段划分已作废，别照它学**，以 roadmap 为准 |

> **分工原则**：`protocol.md` 说"是什么"，`stage-*.md` 说"为什么"，`HANDOFF.md` 说"现在/下一步"，
> `responsibilities.md` 说"谁负责"。**不要在同一处写两遍**——重复的文档一定会互相矛盾。

---

## 阶段进度

| 阶段 | 一句话 | 状态 | 验收项 |
|---|---|---|---|
| S0 | 协议先于代码（帧形状 + golden fixture） | ✅ | protocol.md |
| S1 | 两根管道跑通握手 | ✅ | 12 |
| S2 | 事件流（假模型）—— M1 前置 | ✅ | 33 |
| S3a | 接真模型，非流式 —— **M1** | ✅ | 20 |
| S3b | 真·流式 SSE（分片/码点/重试）—— **M2** | ✅ | 74 + 24 |
| S4 | 工具与 agent 循环 —— **M3** | ✅ | 51 |
| S5 | 打断与并发（取消 + 合批） | ✅ | 24 |
| S6 | 上下文预算与压缩 | ✅ | 28 + 31 |
| **S7** | **会话持久化 / resume / headless** | ⬜ **下一步** | — |
| S8 | 权限、审批与沙箱 | ⬜ | — |
| S9 | TUI —— **M4** | ⬜ | — |
| S10 | MCP 工具 | ⬜ | — |
| S11 | 子智能体 / skills —— **M5** | ⬜ | — |

---

## 架构（一句话版）

```
agent-cli ──request / turn.cancel──► agent-core
   ▲                                    │
   └── event / response ◄───────────────┘
```

- **真相只在 core**：历史、工具、预算、审批判定都在这里。前端不持有任何状态。
- **前端只消费事件**：`assistant.delta` / `tool.call` / `tool.result` / `context.compacted`。
- **协议先于实现**：S0 就把帧形状定死；后面每阶段只**追加**事件和方法，不改旧的。

---

## 铁律（每阶段 DoD 都检查）

1. `agent_common` 绝不链接 HTTP 库或 TUI 库。
2. core 从不向前端发"渲染结果"，只发结构化事件。
3. 前端不认识 HTTP / 工具 / 上下文这些词。
4. **HTTP 只准出现在 `src/core/openai_compat.cpp`**——由 `tests/test_s3a.py::test_layering` 按字面守
   （连注释里出现 `httplib` 都会红）。

已登记的例外：`--embedded` 让 `agent-cli` 在**链接层面**拿到 `agent_core_lib`（含 httplib/OpenSSL），
为的是"同进程直连"的调试能力。源码级铁律不变，见 `docs/stage-s2.md` 决策 2。

---

## 目录

```
src/common/protocol.hpp   构帧 / 解帧（header-only，零网络依赖）
src/core/                 core_engine（协议外壳）/ provider / openai_compat（唯一懂 HTTP 的地方）
                          / sse（纯解析）/ tools（工具层）/ context（预算与压缩）
src/cli/main.cpp          agent-cli：只消费事件的前端
tests/                    ctest 驱动：Python 端到端 + 两个 C++ 单元测试 + fixtures
docs/                     见上面的文档地图
```
