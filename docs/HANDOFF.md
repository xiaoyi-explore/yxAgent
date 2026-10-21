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
| HEAD | tag `s4` 指向的提交（`git log --oneline -1` 就是它） |
| tag | `s0` `s1` `s2` `s3a` `s3b` `s4` |
| 测试 | `ctest` 100%：6 套 —— S1 12 / S2 33 / S3a 20 / S3b 74 / SSE 解析器 24 / S4 51 |
| 真实模型 | DeepSeek `deepseek-chat`：流式（11 增量、首字 0.57s）与**工具循环**（read_file → wc -l → 答 "5"，3 步 2.7s）都已验证 |

## 2. 项目是什么（一句话）

学 C++ 双进程 agent：`agent-core`（大脑：provider / 历史 / 工具 / 权限）+ 前端（现在 `agent-cli`，S9 变 TUI），协议是 NDJSON over stdio。

## 3. 五份"单一真相"文件（每种问题只查一处）

| 文件 | 回答什么问题 |
|---|---|
| `docs/protocol.md` | 线上到底发什么帧（S0 骨架 + S1/S2/S3a/S3b/S4 的追加） |
| `docs/responsibilities.md` | 谁该拥有什么职责、铁律、逐阶段代码地图 |
| `docs/stage-s2.md` / `stage-s3a.md` / `stage-s3b.md` / `stage-s4.md` | 这些阶段为什么这么设计、替代方案、踩了什么坑 |
| `new-agent-stages-v2.md` | 13 个阶段的目标与 DoD（S5 的取消语义在那一节） |
| **本文件** | **现在进行到哪、下一步、怎么验证** |

> S0/S1 没有单独的 `stage-*.md`：S0 的设计就是 `protocol.md` 本身，S1 的记录在 `responsibilities.md` 的「S1 代码地图」。S2 起每阶段都有 `stage-sN.md`。

## 4. 下一步（S5 清单：打断与并发）

1. `turn.cancel` 请求：cli 在等待期间发它，core 立刻停下当前 turn（**仍然回一个正常 response**，`stop_reason=cancelled`），不能只设个标志位了事。
2. **取消必须穿透到 HTTP 层**：`provider.chat_stream` 的 SSE 回调里检查取消标志，返回 `false` 让 httplib 中止读取（`Request::content_receiver` 的返回值就是干这个的）。
3. `assistant.delta` 节流（~30ms 合批）：现在每个 token 都走一次 stdio，长回答会把管道打满。合批后**不变量 2 不变**（拼接仍然等于 result.text）。
4. 只读工具并行、写工具串行（`read_file` 之间没依赖；`write_file`/`run_shell` 必须串行）——先想清线程模型再动手。
5. `tests/test_s5.py`；S1/S2/S3a/S3b/S4 回归必须保持全绿。
6. 更新 `protocol.md` §9 S5、写 `stage-s5.md`、刷新本文件、打 tag `s5`。

## 5. 恢复步骤（新会话 30 秒）

```bash
cd /root/project/yxAgent
git log --oneline --decorate | head -5
cmake --build build 2>&1 | tail -3
cd build && ctest 2>&1 | tail -3
```

全绿（6 套）= 基线完好，直接从 §4 开始。真实模型冒烟（**记得给 --tools**，默认是不给工具的）：

```bash
export DEEPSEEK_API_KEY=$(head -1 /tmp/deepseek.key.bak)   # 或 model/deepseek.md
printf '你好\n' | ./build/agent-cli --base-url https://api.deepseek.com --model deepseek-chat
printf '读一下 a.txt，统计行数\n' | ./build/agent-cli --tools all --workdir /tmp/s4cli --base-url https://api.deepseek.com --model deepseek-chat
```

## 6. 坑与约束（踩过的，别再踩）

- **key 只在服务器** `model/deepseek.md`（`/model/` 已 gitignore）；绝不出现在命令行（`ps` 可见）。
- 同时设 `DEEPSEEK_API_KEY` 和 `OPENAI_API_KEY` 会告警并优先前者——换厂商前先 `unset` 另一个。
- `--embedded` 让 cli 在链接层面拿到 core（含 httplib/OpenSSL）——这是**已登记的例外**；
  源码级铁律由 `tests/test_s3a.py::test_layering` 守：`src/cli/` 里不许出现 httplib / openssl / ftxui。
- **HTTP 只准出现在 `src/core/openai_compat.cpp`**；`src/core/sse.*` 是纯解析、连 httplib 都不链接；
  `src/core/tools.*` 同理（工具层不认识 HTTP）。
- **历史只活在一个 core 进程里**；跨进程持久化是 S7 的事。**失败的一轮整轮回滚**（`history_mark`），不是 pop 一条。
- `--log-raw` 在 core；管道与 embedded 两种模式的抓包必须逐行一致（S2 修过一次）。
- 提交前先看 `git branch --show-current`：之前出现过 detached HEAD 提交（已用 `--ff-only` 修回）。
- **cpp-httplib 的 `Client::Post` 没有 `ContentReceiver` 重载**（只有 `Get` 有）。要边收边解析，
  得自己拼 `httplib::Request` + `req.content_receiver` + `cli.send(req, res, err)`；这条路上 `res.body` 是空的，
  错误正文要自己攒（S3b 踩过，见 `stage-s3b.md`）。
- **流式默认没有 `usage`**：请求体要带 `stream_options: {"include_usage": true}`，否则 `result.usage` 是 `{}`——别编造数字。
- **重试只在"本轮还没吐过任何 delta"时做**（且只覆盖传输层失败）；吐过字再重试会让前端看到重复内容。
- **`tool_calls` 按 `index` 累积**（分片可能乱序、`name` 也会被切开）；`arguments` 拼完才解析，坏 JSON 是常态：
  要变成一次 `ok=false` 的工具结果，**不许抛异常**。
- **`assistant.delta` 必须带 `step`**：一个 turn 有多次模型调用，不带就没法判断"结算时要不要重播全文"。
- **`run_shell` 必须自带超时看门狗**：模型写一句 `sleep 1000` 就能把整个 turn 冻住。
- **模型是无状态的：每次请求都要把整段 `messages` 重新发。**

  系统提示词在 `history_[0]`（`--system` 或内置 agent 提示词），不是每次现拼。
  **光给 `tools` 数组不够**——那只说明"有哪些函数"，不说"什么时候该用"；
  两个都要有。没工具时**不加** system（S1–S3b 的假 server 会数消息条数）。
- `tests/fake_llm.py`（S3a）与 `tests/fake_llm_sse.py`（S3b）是那两阶段的验收基线，**不许改**；S4 用 `fake_llm_tools.py`。

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
4. 提交时信息里带上阶段名，例如 `S5: ...`。

**不要**把设计理由抄进来——那是 `stage-*.md` 的活；本文件只放"现在/下一步/验证/坑"。
