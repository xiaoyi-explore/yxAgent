# S7 —— 会话持久化、恢复与 headless 前端（tag `s7`）

> **一句话**：关掉 cli 再打开，历史还在；并且出现第二个前端 `agent-run`，
> 它只靠协议就能无界面跑完同一个任务。
>
> 这一阶段最重要的不是"写文件"，而是想清楚**什么才算会话的真相**。答案不是协议帧，
> 而是 `history_` 本身；帧只负责审计和 `seq` 水位。

---

## 1. 交付物

| 交付物 | 落点 |
|---|---|
| `~/.agent/sessions/<id>.jsonl` 追加写 | `src/core/session.hpp/.cpp` |
| `session.load` / resume（seq 续号） | `CoreEngine` + 协议 §9 S7 |
| core `--session` / `--resume` / `--session-dir` | `src/core/main.cpp` |
| cli `--session` / `--resume` | `src/cli/main.cpp` |
| `agent-run "<prompt>"` | `src/run/main.cpp`（只链 `agent_common`） |
| 10 套测试 | `tests/test_s7.py`，`ctest` **10/10** |

---

## 2. 关键决定：为什么不是纯协议帧

HANDOFF §5 原来的建议是"格式就用协议帧的 NDJSON，因为帧是真相"。
S7 实现时改成了更准确的说法：

> **帧是线上的真相；会话文件的真相是消息。**

纯帧至少有四处补不回来：

1. **system prompt 从不上线**。它在 `history_[0]`（S4），协议里没有。
2. **`[压缩]` 摘要从不上线**。`context.compacted` 只有计数，摘要正文是 `history_` 里的
   一条 `role:"user"` 消息（S6）。
3. **回滚/取消/MAX_STEPS 是引擎语义**。帧只知道"发了 error response"，不知道
   `history_.resize(history_mark)`；压缩还会把 `history_mark` 往下 clamp。
   从帧还原就得再实现一遍 `chat` 状态机——两份真相，迟早漂移。
4. **`seq`/`turn`/request id 是连接级的**。`turn_` 在失败轮也自增，靠成功响应数推不出来。

所以文件里有两类记录，分工明确：

| 记录 | 是否参与恢复 | 作用 |
|---|---|---|
| `session.*` | **是** | 消息、checkpoint、commit/abort、轮号 |
| `request` / `response` / `event` | **否** | 审计"前端当时看到了什么" + `seq` 水位 |

恢复器不解析任何 delta。**帧可以丢，会话不能丢。**

---

## 3. 磁盘格式（v1）

路径：`~/.agent/sessions/<id>.jsonl`（`--session-dir` 可覆盖；测试用）。
目录 0700，文件 0600。每一行一个 JSON 对象，追加写，UTF-8。

```json
{"type":"session.header","v":1,"id":"s1","created_at":1762243262,"protocol_version":"1.0","core_version":"0.0.1","model":"deepseek-chat","tools":["read_file"],"workdir":"/tmp/x","cwd":"/root/project/yxAgent"}
{"type":"session.checkpoint","turn":0,"reason":"start","at":1762243262,"messages":[{"role":"system","content":"你是一个能使用工具的 agent。…"}]}
{"type":"event","seq":1,"name":"hello","payload":{"protocol_version":"1.0","core_version":"0.0.1","pid":4242}}
{"type":"session.open","resumed":false,"pid":4242,"at":1762243262}
{"type":"request","id":"r2","method":"chat","params":{"text":"统计 a.txt 的行数"}}
{"type":"session.turn.begin","turn":1,"at":1762243263}
{"type":"event","seq":2,"name":"assistant.delta","payload":{"turn_id":"t1","step":1,"text":"我看"}}
{"type":"event","seq":3,"name":"tool.call","payload":{"turn_id":"t1","step":1,"call_id":"call_1","name":"read_file","arguments":{"path":"a.txt"}}}
{"type":"event","seq":4,"name":"tool.result","payload":{"turn_id":"t1","step":1,"call_id":"call_1","name":"read_file","ok":true,"output":"第一行\n第二行\n第三行\n","error":"","duration_ms":1}}
{"type":"event","seq":5,"name":"assistant.delta","payload":{"turn_id":"t1","step":2,"text":"a.txt 共 3 行"}}
{"type":"session.commit","turn":1,"at":1762243264,"stop_reason":"stop","usage":{"prompt_tokens":20,"completion_tokens":8,"total_tokens":28},"steps":2,"tool_calls":1,"messages":[{"role":"user","content":"统计 a.txt 的行数"},{"role":"assistant","content":"我看","tool_calls":[{"id":"call_1","name":"read_file","arguments":"{\"path\":\"a.txt\"}"}]},{"role":"tool","content":"第一行\n第二行\n第三行\n","tool_call_id":"call_1","name":"read_file"},{"role":"assistant","content":"a.txt 共 3 行"}]}
{"type":"response","id":"r2","ok":true,"result":{"turn_id":"t1","text":"a.txt 共 3 行","stop_reason":"stop","usage":{"prompt_tokens":20,"completion_tokens":8,"total_tokens":28},"steps":2,"tool_calls":1}}
```

### 记录表

| 记录 | 何时写 | 说明 |
|---|---|---|
| `session.header` | 建会话第一行 | `v` 是格式版本；配置不含 api key |
| `session.checkpoint` | 建会话 / 压缩改写历史 / 失败回滚后 | 全量 `messages`，append-only 文件里的"重置点" |
| `session.open` | 每次 attach | 审计：谁在何时打开/恢复 |
| `session.turn.begin` | 每轮开始 | `turn` 水位；崩溃也不重用轮号 |
| `session.commit` | 每轮正常结束（含取消） | **一行提交**：本轮新增 `messages`；压缩过的轮带 `checkpoint` 全量 |
| `session.abort` | `PROVIDER_ERROR` / `MAX_STEPS` | 不写消息；压缩过则带回滚后的 checkpoint |
| `request/response/event` | 线上每帧 | 审计 + `seq`；恢复不读 |

### 三个格式决定

1. **一轮一行 commit，而不是一条消息一行**。一行 `fwrite` + `flush` + `fsync` = 原子提交；
   最后一行被截断就整行丢弃，不会出现"半个 turn"的中间态。消息在内存里攒到轮末。
2. **`session.*` 不带 `seq`**，只有 `event` 帧带。这样文件里每个 `seq` 都唯一，
   "seq 不许重号"可以被测试直接扫出来。
3. **`tool_calls.arguments` 存原文 JSON 文本**，不存解析后的对象。模型看到的是文本，
   恢复后原样发回去，坏 JSON 的现场也保留得住。

### 单写者

同一个会话只允许一个写者：打开时 `flock(LOCK_EX|LOCK_NB)`；拿不到就是
`SESSION_BUSY`。S7 不做多写合并，也不做 daemon。两个 cli 开同一个 `--session`，
第二个直接失败——这比"两份日志交错"好排查得多。

---

## 4. 恢复

```text
读所有行，最后一行坏掉就丢（只警告）
header 必须第一行，v 必须 1，否则 SESSION_UNSUPPORTED_VERSION
history = []; last_turn = 0; last_seq = 0

for rec:
  session.checkpoint  -> history = rec.messages
  session.commit     -> if rec.checkpoint: history = rec.checkpoint
                        else:             history += rec.messages
                        last_turn = max(last_turn, rec.turn)
  session.abort      -> if rec.checkpoint: history = rec.checkpoint
                        last_turn = max(last_turn, rec.turn)
  session.turn.begin -> last_turn = max(last_turn, rec.turn)
  event              -> last_seq = max(last_seq, rec.seq)
  其他（request/response/session.open/未知）忽略

turn_ = last_turn
seq_  = max(seq_, last_seq)
```

- **system** 就在 checkpoint 的 `messages[0]`。恢复时不再追加；如果这次 flags 生成的 system
  和会话里不同，stderr 警告并保留会话里的（要换提示词就开新会话）。
- **S6 摘要**没有特殊逻辑：它就是 checkpoint 里一条普通 `role:"user"` 消息，
  `ContextManager::is_summary()` 认 `[压缩]` 前缀，下次压缩会"搬运正文"，不会套娃。
- **工具历史**原样在 `messages` 里：`assistant(tool_calls)` 和 `tool(tool_call_id)` 的配对
  不靠推导，直接读。
- **失败轮**根本不在文件里（`session.abort` 不写 `messages`）；**取消轮**在，
  因为取消是正常结算，半句要留下。
- **压缩后失败**：S6 的压缩不会被 `resize` 回滚（`history_mark` 会被 clamp），
  所以 `session.abort` 要带一份"回滚后的 checkpoint"，否则恢复出来的比内存里多。

### 预算

`ContextManager` 本来就是无状态的，只认 `vector<Message>`：

- 恢复后**没有 token 账本**。下一次 `chat` 进模型前照常调 `ctx_.compact(history_)`，
  它用 `estimate_tokens(history_)` 重新估。如果恢复出来的历史已经过 50% 阈值，
  第一轮就会先压缩再请求。
- `usage` **不从历史累加**。`session.commit` 里的 `usage` 只做审计和显示；
  恢复后第一轮请求的 usage 是这次 provider 给的，从"空"开始。
- `history_mark` 是每轮局部变量，不持久化；`turn_`、`seq_` 从文件恢复。

### seq / 崩溃

- 带会话的连接，`seq` 从 `last_seq + 1` 继续——这是对 `protocol.md §2.3`
  （连接内从 1 开始）的**修订**。不带 `--session` 的连接仍然从 1 开始。
- 每帧经 `log('<')` 落盘并 flush，所以**崩溃前已经发出去的帧，seq 不会在下次恢复时重号**；
  没来得及 commit 的那一轮，只有帧、没有 `session.commit`，恢复时忽略。
- 正常轮：先写 `session.commit`（flush + fsync）→ 再发 response。崩溃在两者之间会出现
  "用户没看到、但历史里已提交"的 ghost turn，这是 WAL 的正常取舍；反过来（先 response
  后落盘）会丢掉用户已经看到的回答，更糟。

---

## 5. 协议增量（详见 `docs/protocol.md` §9 S7）

- `hello` 的 payload 在挂会话时追加 `session` 字段（纯追加，老前端忽略）：
  `{"id","resumed","turns","last_seq"}`。
- 新方法 `session.load`：
  `{"id":"s1","create":true}` 开/建；`{"resume":true}` 取最近；`{"id":"s1"}` 必须已存在。
- 新错误码：`SESSION_NOT_FOUND`、`SESSION_BUSY`、`SESSION_CORRUPT`、
  `SESSION_UNSUPPORTED_VERSION`；非法 id 走 `INVALID_PARAMS`。
- 既有帧形状一个字没改；response 仍是本轮最后一帧。

---

## 6. 两个前端

### cli

```bash
agent-cli --session s1 ...          # 开/建
agent-cli --resume ...              # 最近
agent-cli --resume=s1 ...           # 指定
agent-cli --session-dir DIR ...     # 测试/多环境
```

握手后 stdout 打一行 `[session] new/resumed s1 (turns=N, last_seq=M)`。

### agent-run

```bash
agent-run --tools all --workdir /tmp/x "统计 a.txt 的行数"
agent-run --resume=s1 "继续"
```

- **stdout 只放最终答复**（`result.text`），工具进度 / `context.compacted` / 错误走 stderr。
- 退出码：成功 0；参数/握手/provider 1；core 中途死 3。
- **只 include `common/protocol.hpp`，只链接 `agent_common`**。它不 include 任何
  `core/` 头、不碰 httplib/openssl、不支持 `--embedded`——"同一 core，两个前端"由
  构建系统和 `tests/test_s7.py` 的 grep 共同守着。

---

## 7. 测试

`tests/test_s7.py` 10 组：

| # | 用例 | 验证 |
|---|---|---|
| 1 | 落盘格式 | header v1、checkpoint/commit、event seq 唯一递增、0600 |
| 2 | 历史恢复 | 重启后请求里 system/user/assistant 都在且不重复 |
| 3 | seq 与 turn | 跨连接续号、turn_id t3、文件内 seq 无重号 |
| 4 | 压缩摘要恢复 | `[压缩]` 在 checkpoint 里、重启后仍在且不套娃 |
| 5 | 工具历史恢复 | assistant(tool_calls) + tool 配对、无孤儿 tool |
| 6 | 失败轮 | `session.abort`、不进历史、重启后不污染 |
| 7 | 轮中崩溃 | 未提交轮被忽略、seq 不复用 |
| 8 | session.load | create/load/resume/错误码/幂等 |
| 9 | cli resume | `--session` → `--resume`，模型看到上一句 |
| 10 | agent-run | 退出 0、stdout 干净、resume、分层 grep |

`ctest`：**10/10**（S1–S6 九套 + S7 一套）；`test_s7.py` 共 **107** 项 PASS。

---

## 8. 边界与欠账

- **S7 是 lean 版**：只在轮末 commit。轮中 SIGKILL 会丢掉整轮（包括 user 消息）。
  DSH 在模型请求前、工具副作用前各 checkpoint，并在恢复时补 `TOOL_OUTCOME_UNKNOWN`；
  Claude/Codex 也会检测中断轮。**DSH 式 checkpoint 留给 S8**（审批/危险命令本来就要
  在执行前落盘），到那时再把 `session.commit` 拆成 begin/tool/result 更划算。
- **压缩用 checkpoint，不学 DSH 的 replace 投影**。S6 的 `history_` 是可变 vector，
  改成"保留全部事件 + 投影遮蔽"要动 S6 的语义；S7 先接受"checkpoint 重置"。
- **`session.load` 的请求帧不落盘，response 落到新会话**：切换会话时请求属于旧会话、
  响应属于新会话，干脆不写请求，避免两边都脏。
- **单写者不合并**：第二个进程 `SESSION_BUSY`。以后要并发分支，加 `fork`，不要偷偷多写。
- **`~/.agent` 没有清理策略**：会话文件会累积，S7 不做 GC。
- **`--resume` 取最近**依据 `sessions/last`；文件丢了会退回按 mtime 扫。

---

## 9. 替代方案（为什么没选）

| 方案 | 为什么不选 |
|---|---|
| 纯协议帧 | 丢 system、摘要、回滚语义；恢复器要重写一遍引擎 |
| 纯消息、不存帧 | 丢 seq 水位 / 审计；S7 的"seq 不重号"没法直接验 |
| DSH 式事件溯源 + surface 投影 | 更稳，但要动 S6 的 `history_` 模型和版本迁移，超出 S7 |
| SQLite 做会话主存 | 查询方便，但 S7 要的是可读、可 `jq`、可 diff 的日志；Codex/DSH 也是 JSONL 主存 + SQLite 投影 |

---

## 10. DoD

- [x] 关掉 cli 再开（同 `--session`），历史还在，且能接着上一句问
- [x] `agent-run "统计 a.txt 的行数"` 无界面完成任务，退出码 0
- [x] 同一 core 二进制，两个前端，零改动（只给 hello 加了可忽略的 `session` 字段）
- [x] S1–S6 九套仍全绿，新增 `tests/test_s7.py` → `ctest` 10/10
- [x] `protocol.md` §9 S7、本文件、刷新 `HANDOFF.md`、打 tag `s7`