# HANDOFF —— 会话交接与进度真相

> **新会话的第一件事：读本文件 + `git log --oneline --decorate`。**
> 本文件只回答四件事：**现在在哪、下一步做什么、怎么验证、有哪些坑**。
> 每次阶段结束（或上下文将尽）必须刷新它——它比聊天记录长寿。

---

## 1. 现在在哪（快照）

| 项 | 值 |
|---|---|
| 仓库 | `/root/project/yxAgent`（服务器 `111.228.29.170`） |
| 分支 | `main` |
| HEAD | `caaded3`（tag `s3a`） |
| tag | `s0` `s1` `s2` `s3a` |
| 测试 | `ctest` 100%：S1 12 项 / S2 33 项 / S3a 20 项 |
| 真实模型 | DeepSeek `deepseek-chat` 已验证（单轮 0.95s、多轮记忆 42） |

## 2. 项目是什么（一句话）

学 C++ 双进程 agent：`agent-core`（大脑：provider / 历史 / 工具 / 权限）+ 前端（现在 `agent-cli`，S9 变 TUI），协议是 NDJSON over stdio。

## 3. 五份"单一真相"文件（每种问题只查一处）

| 文件 | 回答什么问题 |
|---|---|
| `docs/protocol.md` | 线上到底发什么帧（S0 的骨架 + S1/S2/S3a 的追加） |
| `docs/responsibilities.md` | 谁该拥有什么职责、铁律、逐阶段代码地图 |
| `docs/stage-s2.md` / `docs/stage-s3a.md` | 这两个阶段为什么这么设计、替代方案、踩了什么坑 |
| `new-agent-stages-v2.md` | 13 个阶段的目标与 DoD（S3b 的六条 fixture 在这里） |
| **本文件** | **现在进行到哪、下一步、怎么验证** |

> S0/S1 没有单独的 `stage-*.md`：S0 的设计就是 `protocol.md` 本身，S1 的记录在 `responsibilities.md` 的「S1 代码地图」。S4 起按 DoD 第 8 条补 `stage-sN.md`。

## 4. 下一步（S3b 清单）

1. `OpenAIConfig` 加 `stream` 开关；`/chat/completions` 发 `stream: true`（S3a 的 `false` 留作非流式回退）。
2. 新增 `src/core/sse.hpp/.cpp`：按行缓冲 → 空行派发 → `data:` 多行拼接 → `[DONE]` 收尾；**绝不假设 chunk 落在事件边界**。
3. `Provider` 长出新方法（如 `chat_stream(messages, on_delta)`），engine 在循环里 emit `assistant.delta`；**响应形状不变**（S2 的"响应最后结算"继续有效）。
4. 六条 fixture 全部覆盖（见计划文档 S3b 节）：JSON 中间切 / UTF-8 码点中间切 / `:` 注释行 / 慢流 / 断连 / usage 缺失（OpenAI 默认流里不带 usage，要 `stream_options.include_usage`）。
5. `tests/test_s3b.py`；S1/S2/S3a 回归必须保持全绿。
6. 更新 `protocol.md` §9 S3b、写 `stage-s3b.md`、刷新本文件、打 tag `s3b`。

## 5. 恢复步骤（新会话 30 秒）

```bash
cd /root/project/yxAgent
git log --oneline --decorate | head -5
cmake --build build 2>&1 | tail -3
cd build && ctest 2>&1 | tail -3
```

三次全绿 = 基线完好，直接从 §4 开始。真实模型冒烟：

```bash
export DEEPSEEK_API_KEY=$(head -1 /tmp/deepseek.key.bak)   # 或 model/deepseek.md
printf '你好\n' | ./build/agent-cli --base-url https://api.deepseek.com --model deepseek-chat
```

## 6. 坑与约束（踩过的，别再踩）

- **key 只在服务器** `model/deepseek.md`（`/model/` 已 gitignore）；绝不出现在命令行（`ps` 可见）。
- 同时设 `DEEPSEEK_API_KEY` 和 `OPENAI_API_KEY` 会告警并优先前者——换厂商前先 `unset` 另一个。
- `--embedded` 让 cli 在链接层面拿到 core（含 httplib/OpenSSL）——这是**已登记的例外**；
  源码级铁律由 `tests/test_s3a.py::test_layering` 守：`src/cli/` 里不许出现 httplib / openssl / ftxui。
- **历史只活在一个 core 进程里**；跨进程持久化是 S7 的事。
- `--log-raw` 在 core；管道与 embedded 两种模式的抓包必须逐行一致（S2 修过一次）。
- 提交前先看 `git branch --show-current`：之前出现过 detached HEAD 提交（已用 `--ff-only` 修回）。

## 7. 上下文预算策略（本文件存在的理由）

- **每阶段结束必刷新本文件**，不要等上下文将尽。
- 上下文快满时：**先写本文件，再结束会话**——"先写遗嘱再断电"。写清楚：正在做什么、卡在哪、下一条命令。
- 新会话别只说"继续"：说「读 `docs/HANDOFF.md`，从 §4 继续」。
- **不要指望聊天记录**：新会话看不到它；仓库才是持久记忆。

## 8. 怎么更新本文件（30 秒模板）

只改四行 + 必要时改 §4：

1. §1 的 HEAD / tag / 测试数字（跑一遍 §5 就知道）。
2. §4 换成"下一步"的真实清单（当前阶段没做完就原样保留）。
3. §6 追加本阶段新踩的坑（一条就够，别写成流水账）。
4. 提交时信息里带上阶段名，例如 `S3b: ...`。

**不要**把设计理由抄进来——那是 `stage-*.md` 的活；本文件只放"现在/下一步/验证/坑"。
