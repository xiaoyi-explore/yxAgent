# S3b —— 升级成 SSE 流式（里程碑 M2）

> 上游计划：`roadmap.md` 的 S3b 节。本文件记录**为什么这么设计、替代方案、踩了什么坑**。
> 协议变化见 `docs/protocol.md` §9 S3b。现在在哪 / 下一步见 `docs/HANDOFF.md`。

---

## 1. 一句话

**S2 定了事件流长什么样，S3b 负责从 HTTP 分片里把这些事件还原出来。**
帧的形状一个字没改——cli 的渲染代码也没改（只删掉了"结算时重播整段"那一行）。

交付物：`src/core/sse.hpp/.cpp`（增量解析器）、`OpenAICompatProvider::chat_stream`、
`Provider` 接缝上的 `chat_stream/DeltaSink`、`--no-stream` 回退开关、
`tests/fake_llm_sse.py` + `tests/fixtures/sse_*.sse` + `tests/test_s3b.py` + `tests/sse_parser_test.cpp`。

---

## 2. 五个决策（每个都有替代方案）

### 决策 1：解析器的工作单位是"行"，不是"事件"

`SseParser::feed()` 只做三件事：追加字节 → 能凑出一整行就处理这一行 → 空行则派发事件。

为什么不是"找 `\n\n` 再切"：`\r\n\r\n`、纯 `\r`、以及**切在 `\r` 和 `\n` 之间**的 chunk
会把它切成"看起来没有终止符"的假象。以行为单位就没这个问题——`\r` 落在 buffer 末尾时无法判断
后面跟不跟 `\n`，那就**等下一段**；真结束了由 `flush()` 收尾。

**替代方案**：一次性把整个 body 收完再正则切分（实现最简单）。
否决理由：那就不是流式了——慢速流会在前端表现为"卡住，然后一次全出来"，正好是 S3b 要消灭的体验。

### 决策 2：默认流式；对端不配合时自动回退

`OpenAIConfig::stream` 默认 `true`。请求发出后：
* 收到了 `data:` 里能解析成 JSON 的事件 → 按 SSE 走；
* 一个事件都没收到（正文不是 SSE）→ 把攒下的正文当**整段 JSON** 解析，把全文当一个增量吐出去。

于是"网关无视 `stream:true`、直接返回整段 JSON"这种真实情况不会变成错误，而只是"没有打字机效果"。
这条回退也让 S3a 的假 server（永远回整段 JSON）在 S3b 之后**一行都不用改**——S3a 的 20 项断言原样通过。

**替代方案**：看 `Content-Type` 决定走哪条路。否决理由：httplib 把响应头交给回调的时机与
`Request::content_receiver` 的路径不完全一致，而"有没有 `data:` 事件"是**行为**证据，比声明更硬。

### 决策 3：重试只覆盖"传输层失败"，且只在还没吐字时

`chat_stream()` 的循环：`stream_once()` 抛 RetryableError 且 `emitted == false` → 退避重试一次。
可重试集合 = 连不上 / 连接被掐断 / 读超时（= **这个请求根本没落地**）。

**不重试 HTTP 状态码**（401/429/5xx），理由不是"不想做"，而是**契约冲突**：
S3a 的 `tests/test_s3a.py::failonce` 用"第一次 500、之后正常"来验收"失败的一轮不回灌历史"。
如果 5xx 自动重试，这条验收就变成不可测（第一轮直接成功了）。状态码级的重试/退避属于 S5 的
"取消与重试策略"，那时它会有自己的开关和可见性，而不是在这里偷偷发生。

**吐过字绝不重试**：用户会看到重复的半句话，比报错更糟。中途断连（`/abort` fixture）因此
如实变成 `PROVIDER_ERROR`（或"只交付断之前收到的那部分"，取决于传输层是否已经报错）。

### 决策 4：测试分两层，一层穷举、一层端到端

* `tests/sse_parser_test.cpp`（纯 C++，只链接 `sse.cpp`）：对每个 fixture 做**穷举切点**——
  1 字节一喂、2 字节一喂……一次喂完，比对事件序列是否完全一致；再把 fixture 转成 CRLF 重跑一遍。
  这是**确定性**的：它证明"切在哪都不影响结果"，而这正是 TCP/TLS 会做的事。
* `tests/test_s3b.py`（Python，起假 server）：证明 HTTP 层真的在流、真的忽略注释行、
  真的处理状态码与断连。

**只有第二层是不够的**：靠 sleep 控制 chunk 边界永远有竞态，测试会变成"大部分时候过"。
**只有第一层也是不够的**：它证明不了解析器外面的接线（请求体、状态码、重试、cli 渲染）。

顺带的好处：`sse_parser_test` 只编译 `sse.cpp`，不链接 httplib/OpenSSL——
"SSE 解析器不认识 HTTP"这条铁律于是**由构建系统机器化验证**，不靠自觉。

### 决策 5：假 server 仍用 Python（偏离计划文档一处，理由在此）

计划文档写的是"基于 `httplib::Server` 回放 fixture"。实际做成了 `tests/fake_llm_sse.py`
（`ThreadingHTTPServer` + 手写 chunked），理由：

1. 仓库里 S3a 的假 server 已经是 Python，`ctest` 的驱动也是 Python；再引入一个 C++ server
   就多一种启动/收尸方式，收益为零。
2. **RST 断连**（`SO_LINGER=0` + `close`）在 Python 里三行，在 httplib::Server 里做不到干净。
3. 我们需要的是"按我的切法发字节"，`wfile.write + flush` 比 httplib 的 chunked provider 更直接。

fixture 仍是**可读的字节文件**（`tests/fixtures/sse_*.sse`），只是允许 `{{MSGS}}` / `{{ECHO}}`
两个占位符，好让"多轮历史"能被验证。

---

## 3. 坑（下一个会话别再踩）

1. **cpp-httplib 的 `Client::Post` 没有 `ContentReceiver` 重载**——只有 `Get` 有。
   想边收边解析必须自己拼 `httplib::Request`（把回调塞进 `req.content_receiver`），
   再走 `cli.send(req, res, err)`。写成 `cli.Post(..., receiver)` 是编译错误。
   而且这条路径下 `res.body` 是空的，错误正文要自己攒（代码里叫 `raw`）。
2. **`tests/test_s3a.py` 的 `chat()` 必须跳过事件帧**：它原来假设"第一个读到的帧就是响应"，
   而 S3b 之后 `chat` 会先吐 `assistant.delta`。改动只有 "跳过 event 帧" 这四行，
   **20 条断言一个字没改**——这是"回归全绿"的正确形态。
3. **不许改 `tests/fake_llm.py`**：它是 S3a 的验收基线。S3b 另起 `fake_llm_sse.py`。
4. **`usage` 默认不在流里**：OpenAI 兼容端点要在请求体里显式写
   `stream_options: {"include_usage": true}`，否则最后一帧没有 usage（fixture `sse_nousage.sse`
   就是这件事）。`result.usage` 因此在流式下可能是 `{}`——**不编造数字**。
5. **`finish_reason` 在 `choices[i].finish_reason`，不在 `delta` 里**。
   写进 delta 就成了"自己发明字段"，解析器（正确地）看不见它，`stop_reason` 会是空串。

---

## 4. 验收（怎么证明它成了）

```bash
cd /root/project/yxAgent
cmake --build build
cd build && ctest --output-on-failure     # 5 套：S1 / S2 / S3a / S3b-SSE-解析器 / S3b-流式
```

| 套件 | 检查项 |
|---|---|
| `s1_handshake` | 12 |
| `s2_stream` | 33 |
| `s3a_provider` | 20（**一项没改**，只改了 `chat()` 的读取方式） |
| `s3b_sse_parser` | 24（穷举切点，含 CRLF 变体） |
| `s3b_stream` | 74 |

真模型冒烟（实测：11 个增量、首字 0.57s、`concat(text) == result.text`）：

```bash
export DEEPSEEK_API_KEY=$(head -1 /tmp/deepseek.key.bak)
printf '你好，用一句话介绍你自己\n' | ./build/agent-cli --base-url https://api.deepseek.com --model deepseek-chat
```

---

## 5. 刻意不做

* 状态码级重试/退避、`Retry-After`（S5）。
* 取消（Ctrl-C 打断流）——需要穿透到 HTTP socket，S5。
* 工具调用的流式分片累积（`tool_calls[].function.arguments` 按 index 拼）——S4 的头号坑，
  到那时再做，见计划文档 S4 节。
* 多轮里"半截流"进历史的问题：失败会回滚，成功则整段入历史（S7 落盘时再考虑校验）。
