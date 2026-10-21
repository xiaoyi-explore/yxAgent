# S4 —— Agent 循环与工具调用（里程碑 M3）

> 上游计划：`new-agent-stages-v2.md` 的 S4 节。本文件记录**为什么这么设计、替代方案、踩了什么坑**。
> 协议变化见 `docs/protocol.md` §9 S4。现在在哪 / 下一步见 `docs/HANDOFF.md`。

---

## 1. 一句话

**S3b 让模型能说话，S4 让它有手脚。** 一个 `chat` 请求从"一次 HTTP 调用"变成一个循环：

```
loop:
  resp = model(messages, tools)          # 带着工具声明去问
  没有 tool_calls ?  → 收尾，这就是最终答复
  有 tool_calls     ?  → 逐个执行：emit tool.call → 执行 → emit tool.result → 结果回灌历史
  step > MAX_STEPS  ?  → 中止（MAX_STEPS）
```

交付物：`src/core/tools.hpp/.cpp`（注册表 + read_file / write_file / run_shell）、
`Provider` 接缝上的 `ToolSpec` / `ToolCall` / `ChatRequest`、engine 里的循环与 `MAX_STEPS`、
`--tools / --workdir / --max-steps`、`tests/fake_llm_tools.py` + `tests/test_s4.py`。

---

## 2. 七个决策（每个都有替代方案）

### 决策 1：循环放在 `chat` 里，不新开一个方法

`chat` 的请求/响应形状一个字没改，只是"中间可能长出工具事件"。前端（cli）因此不用切换方法，
S1–S3b 的测试也一条都不用改。
**替代方案**：新增 `agent.chat` 方法、`chat` 保持单步。否决理由：那等于让前端记住"什么时候用哪个方法"，
而"要不要调工具"是模型自己的决定，不是调用方的决定。

### 决策 2：工具**默认关闭**，`--tools all` 才装上

能改文件、能执行任意 shell 的能力不该被顺手打开。S4 没有审批层（那是 S8），
所以"默认不给"就是这个阶段的阀门；也给 S1–S3b 的回归留了一个干净的等价路径。

### 决策 3：provider 只管 wire format，参数解析在循环里

`ToolCall.arguments` 保持**字符串**（模型给的原文），provider 一个字节都不解释。
解析、校验"是不是 JSON 对象"、失败怎么告诉模型，全在 engine。
理由：换一家 provider，tool_calls 的编码会变，但"参数是 JSON 文本"这件事不变；
而"坏 JSON 怎么办"是 agent 的语义，不是协议适配层的事。

### 决策 4：工具执行是**同步**的，但命令要带看门狗

ReAct 天然是一步接一步，S4 不做并发（并行只读工具是 S5 的内容）。
但同步有个致命风险：模型写一句 `sleep 1000`，整个 turn 就冻住了——
所以 `run_shell` 用 `fork + exec + waitpid(WNOHANG)` 自己实现超时（默认 30s，上限 120s），
到期 `SIGKILL` 并如实报告 `timeout`。另外 stdout/stderr 合并进一个管道，边跑边排空，
避免子进程写满 64KB 管道缓冲区后卡死。

### 决策 5：工具失败**也要回灌**历史

`ok=false` 时回灌 `ERROR: <原因>` + 原始输出。这是模型自我纠正的唯一线索：
它读到"文件不存在"才会换个路径，而不是编一个答案。整个 turn 只有在**模型/HTTP**出错时才失败。

### 决策 6：失败的一轮从"整轮回滚"，不是 pop 一条

S3a 的契约是"失败的一轮不污染下一次请求"。S4 之后一轮可能往历史里塞 1 + 1 + N 条消息
（user / assistant(tool_calls) / 每个工具的 tool 消息），`pop_back()` 一条已经不够了，
所以改成进 turn 前记 `history_mark`，出错时 `resize(history_mark)`。
`MAX_STEPS` 同样整轮回滚（卡住的循环本身是 bug，留着半截历史只会让它继续卡）。

### 决策 7：系统提示词放在 `history_[0]`，不是每次请求现拼

模型是**无状态**的：每次请求都要把整段 `messages` 重新发一遍（这是 OpenAI 协议的全部秘密，
也是"上下文"这个词的真实含义）。所以"你能用工具"这件事必须在**每一份**请求里重新出现。

那它该放在哪？选了三处里最不起眼但最正确的一处——**历史的第一条**：

| 做法 | 问题 |
|---|---|
| 每次请求时现拼一个 system 塞进 body | 它不在"对话真相"里：S7 落盘会漏掉它，S6 压缩也不知道该不该保它 |
| 放 cli，由前端拼进 `params` | 违反铁律 2/3：前端不该知道"agent 该怎么说话" |
| **放 `history_[0]`（本阶段选择）** | 每次请求自动带上，回滚（`resize(mark)`）也滚不掉它，S7 落盘自带 |

内容上分两层，**不要互相重复**：

* `tools` 数组说"**有哪些函数、参数是什么**"（JSON Schema，机器可验证）；
* 系统提示词说"**什么时候该用、结果可不可信、别瞎猜**"（行为约束，模型才听得懂）。
  只有前者时，模型经常"知道有 read_file 却直接编一个答案"。

开关：`--system TEXT` 覆盖；不给时**有工具就用内置 agent 提示词**，
**没工具就一条 system 都不加**——后者是刻意的：S1–S3b 的假 server 会数消息条数
（`fake(1 msgs)`），凭空多一条 system 会让它们全红，而"回归必须全绿"比"多带一句人设"重要。

---

## 3. 坑（下一个会话别再踩）

1. **`tool_calls` 必须按 `index` 累积，不是按数组下标。** 分片可能乱序到达（先 index=1 再 index=0），
   同一个 index 的 `arguments` 要**字符串拼接**；`name` 也可能被切开（测试里故意切成两片）；
   `id` 只来一次。用 `std::map<int, ToolCall>` 收，最后按 key 序展开。
2. **`assistant.delta` 必须带 `step`。** 不带的话，前端会把"我看看"和最终答案黏成一行；
   结算时也会算错"要不要重播全文"——判断依据是"**最后一个 step** 有没有吐过字"。
3. **坏 JSON 参数不能抛异常。** 模型的幻觉参数是常态，engine 必须把它变成一次 `ok=false` 的工具结果，
   并原样（截断后）回显 `arguments_raw` 给模型看。
4. **工具结果不要 dump 成 JSON 喂给模型。** `{"ok":true,"output":...}` 会白烧 token 也容易误读；
   回灌的 `content` 是人/模型都读得懂的一段文本（失败时以 `ERROR: ` 开头）。
5. **`run_shell` 不设超时 = 把 agent 变成砖。** 见决策 4。
6. **`Tool::run` 用 `const`。** `ToolRegistry::execute` 拿到的是 `const Tool*`，写成非 const 直接编译不过
   （第一次构建就撞上了）。
7. **别以为"给了 tools 就等于告诉模型能用工具"。** 协议层的 tools 只是一张函数签名表；
   "该不该用、结果可不可信"必须在系统提示词里说。第一版 S4 漏了这条，真模型照样能跑，
   但它是在"自己猜出来能用工具"——换个模型（或换个温度）就会退化。

---

## 4. 验收（怎么证明它成了）

```bash
cd /root/project/yxAgent
cmake --build build
cd build && ctest --output-on-failure     # 6 套全绿
```

| 套件 | 检查项 |
|---|---|
| `s1_handshake` | 12 |
| `s2_stream` | 33 |
| `s3a_provider` | 20 |
| `s3b_sse_parser` | 24 |
| `s3b_stream` | 74 |
| `s4_agent_tools` | 51 |

真模型 DoD（实测 3 步 / 2 个工具 / 2.7s：先 `read_file`，再自己补一个 `wc -l`，最后答 "5"）：

```bash
export DEEPSEEK_API_KEY=$(head -1 /tmp/deepseek.key.bak)
printf '读一下 a.txt，统计它有多少行，只回答行数。\n' | ./build/agent-cli --tools all --workdir /tmp/s4cli\n  --base-url https://api.deepseek.com --model deepseek-chat
```

cli 的输出形状（工具行 + 打字机答案 + 结算行）：

```
[core ready] protocol=1.0 core=0.0.1 pid=828914
[handshake] ok
[tool] run_shell {"command":"wc -l a.txt"} -> ok 5ms
[assistant] 3 行
[turn t1 done: stop_reason=stop, tokens=632/3/635, steps=2, tools=1]
[core exited cleanly, code=0]
```

---

## 5. 刻意不做

* 审批 / allow-ask-deny（S8）：本阶段唯一的阀门是 `--tools` 默认关闭。
* 取消（S5）：跑起来的工具不能被中途打断——需要把信号传进 `waitpid` 循环，S5 一起做。
* 并行只读工具（S5）：`read_file` 之间没有依赖，可以并行；但要先有线程模型。
* 路径沙箱 / 工作目录越界检查（S8）：现在相对路径锚在 `--workdir`，绝对路径原样放行。
* 工具结果裁剪与上下文预算（S6）：现在只截 64KB 输出 + 200KB 读文件，没有 token 预算概念。