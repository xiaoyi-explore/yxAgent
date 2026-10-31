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
| HEAD | tag `s5` 指向的提交（`git log --oneline -1` 就是它） |
| tag | `s0` `s1` `s2` `s3a` `s3b` `s4` `s5` |
| 测试 | `ctest` 100%：**7 套** —— S1 12 / S2 33 / S3a 20 / S3b 74 / SSE 解析器 24 / S4 51 / S5 24 |
| 真实模型 | DeepSeek `deepseek-chat`：流式（11 增量、首字 0.57s）、**工具循环**（read_file → wc -l → 答 "5"，3 步 2.7s）已验证；S5 的取消用假 server（`/manyslow`）验，真模型手动 Ctrl-C 待冒烟 |

## 2. 项目是什么（一句话）

学 C++ 双进程 agent：`agent-core`（大脑：provider / 历史 / 工具 / 权限）+ 前端（现在 `agent-cli`，S9 变 TUI），协议是 NDJSON over stdio。

## 3. 五份"单一真相"文件（每种问题只查一处）

| 文件 | 回答什么问题 |
|---|---|
| `docs/protocol.md` | 线上到底发什么帧（S0 骨架 + S1/S2/S3a/S3b/S4 的追加） |
| `docs/responsibilities.md` | 谁该拥有什么职责、铁律、逐阶段代码地图 |
| `docs/stage-s2.md` / `stage-s3a.md` / `stage-s3b.md` / `stage-s4.md` / `stage-s5.md` | 这些阶段为什么这么设计、替代方案、踩了什么坑 |
| `new-agent-stages-v2.md` | 13 个阶段的目标与 DoD（S6 的 token 预算在那一节） |
| **本文件** | **现在进行到哪、下一步、怎么验证** |

> S0/S1 没有单独的 `stage-*.md`：S0 的设计就是 `protocol.md` 本身，S1 的记录在 `responsibilities.md` 的「S1 代码地图」。S2 起每阶段都有 `stage-sN.md`。

## 4. 下一步（S6：token 预算与压缩）

> 目标细节读 `new-agent-stages-v2.md` 的 S6 节；本文件只列"开始前必须知道的三件事"。

1. **预算是硬约束，不是估算**：`usage` 是唯一可信的账本（S3b 起流式也带了 `include_usage`）。
   `result.usage` 缺失时**不许编造**（现在会对端不给就是 `{}`，cli 显示 `0/0/0` —— 这是 S6 该顺手修的）。
2. **压缩只能动历史，不能动协议**：`history_` 在 core，压缩后客户端**看不见任何差别**（帧形状不变）。
   如果将来需要前端知情（比如显示"历史被压缩了"），那是一个新 event，不是改 response。
3. **失败/取消的既有契约不许破**：失败整轮回滚（`history_mark`）、取消保留已产出部分——压缩必须和这两条共存。

顺带欠账（S5 明确推掉的，别忘）：

- **只读工具并行、写工具串行**——需要真正的线程模型（或 `fork` + 非阻塞 `waitpid` 调度），
  和"取消"是两件事，所以 S5 保持单线程。做它之前先想清楚"谁能写 stdout"。
- `--embedded` 仍不支持取消（同步回调、单线程，没有等响应时读输入的窗口）。
- 真模型的取消冒烟还没做（只跑了假 server）。

## 5. 恢复步骤（新会话 30 秒）

```bash
cd /root/project/yxAgent
git log --oneline --decorate | head -5
cmake --build build 2>&1 | tail -3
cd build && ctest 2>&1 | tail -3
```

全绿（7 套）= 基线完好，直接从 §4 开始。真实模型冒烟（**记得给 --tools**，默认是不给工具的）：

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
- **要 `poll` 的东西不能让 stdio 帮它缓冲。** cli 第一版用 `getline(FILE*)` + `poll(fd)`：
  `getline` 把后面的行读进 stdio 缓冲区，`poll` 就再也不亮 —— `test_s2` 直接 30s 超时。
  读 core 的 stdout 一律走裸 fd + 自己的行缓冲（`read_line` 返回 1/0/-1）。
- **取消时"先收下再喊停"**：provider 在调 `on_delta` 之前已经把这一片算进 `result.text` 了；
  引擎要是没接这一片就返回 false，结算文本会比 delta 拼接多一个字 —— **不变量 2 当场破**。
- **主动断流时先判 `aborted` 再判 `!sent`**：掐流时 HTTP 客户端一定返回 false + `Canceled`，
  判反了用户按 Ctrl-C 会看到 `PROVIDER_ERROR`。
- **`test_s3a.py::test_layering` 是按字面 grep 的**：连注释里出现 `httplib` 都会红。
  想不红就去掉那个词（改成"HTTP 客户端"），别改测试。
- **`demo.chat` 不合批**：S2 的验收是"一个 delta 恰好一个码点"，新阶段不许偷偷改老契约。
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
