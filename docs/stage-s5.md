# S5 —— 打断与并发（里程碑 M4）

> 上游计划：`roadmap.md` 的 S5 节。本文件记录**为什么这么设计、替代方案、踩了什么坑**。
> 协议变化见 `docs/protocol.md` §9 S5。现在在哪 / 下一步见 `docs/HANDOFF.md`。

---

## 1. 一句话

**S4 让 core 会干活，S5 让用户能喊停。** 一个 turn 从"必须等它跑完"变成"随时可以中断"，
而且这个中断**不是在前端假装**——它一路穿到 TCP：HTTP 客户端的 `content_receiver` 返回 false，
这次传输当场被掐掉。顺带把"一字一帧"的增量合批成 ~30ms 一帧。

交付物：`turn.cancel` 方法、`DeltaSink` 的 bool 返回值、`Options.delta_batch_ms`、
cli 的 Ctrl-C、`tests/test_s5.py` + `tests/fixtures/sse_many.sse` + fake server 的 `many/manyslow` 模式。

---

## 2. 五个决策

### 决策 1：取消用 `poll` + 非阻塞读，**不引入线程**

问题的形状：core 卡在 HTTP 读上时，怎么看见 cli 刚写进来的 `turn.cancel`？

| 方案 | 做法 | 评价 |
|---|---|---|
| A. 读线程 + 请求队列 | 一个线程专职读 stdin，主线程跑 turn | 能解决，但引入"谁能写 stdout"、"退出时怎么 join 一个卡在 getline 的线程"两串新问题——**调试工具自己成了 bug 源**（S2 决策 2 用过的同一条理由） |
| B. 自管道 + poll | 主线程 poll {stdin, wake-pipe} | 干净，但要多一个 fd 和一条唤醒协议；turn 内部的代码没有地方 poll |
| C. **非阻塞 peek（采用）** | `run()` 用 poll+read 自己切行；**增量回调里顺手 `pump_input()` 一次**（timeout=0） | **零线程、零队列**；取消延迟 = 下一个增量的间隔（实测 50ms 一个的流里几乎瞬时） |

C 还顺手解决了"每一帧都要能被取消检查"这件事：**增量回调是这一轮里最频繁发生的事件**，
把检查挂在它身上，等于自动获得了"节流级别的响应速度"。

代价（必须登记）：

- **`--embedded` 不支持取消**：它是同步回调、单线程，没有"等响应时还能读输入"的窗口。
  这是 embedded 一贯的定位（降级调试通道），不是缺陷。
- **非流式（`--no-stream`）只能在"模型调用返回之后"停**：POST 没有 `content_receiver`，
  掐不断。响应仍然是 `stop_reason=cancelled`，只是它可能在模型已经说完之后才生效。
- **`run()` 的签名从 `run(istream&, ostream&)` 改成 `run()`**：`poll` 只认真实 fd。
  之前那个签名里的 `in`/`out` 从来没用过——**签名撒谎比签名短更糟**。

### 决策 2：取消 **≠** 失败：保留已经发生的部分

S3a/S4 的规则是"失败的一轮整轮回滚（`history_.resize(history_mark)`）"。取消**故意不**走这条路。

| 方案 | 结果 |
|---|---|
| 整轮回滚（连 user 一起丢） | 用户问过的话从历史里消失，下一轮模型不知道他问过——**最糟** |
| 只留 user、丢掉半句回答 | 历史变成 `[user, user]`：**两个连续的 user**，正是 S3a 立回滚规矩时想消灭的那种污染 |
| **保留 user + 已产出的半句（采用）** | 屏幕上看到的和模型记得的一致；它是 `[user, assistant(部分)]`，**结构合法** |

所以取消分支里只做一件事：`if (!r.text.empty()) history_.push_back(assistant)`。
"这句话不作数"这件事**由 `stop_reason=cancelled` 告诉前端**，不靠偷偷改历史。

### 决策 3：`DeltaSink` 返回 `bool`，取消一路穿到 TCP

```cpp
using DeltaSink = std::function<bool(const std::string& delta)>;  // false = 别读了
```

链条：引擎的 `on_delta` 返回 false → provider 记下 `aborted` → `content_receiver` 返回 `!aborted`
→ HTTP 客户端中止这次传输。**这才是"穿透到 HTTP"，不是"发完再丢掉"。**

两个必须写对的细节（都踩过）：

1. **先收下，再喊停。** provider 在调用 `on_delta` **之前**已经把这一片加进 `out.text` 了。
   如果引擎在这一片还没进 `pending` 时就返回 false，结算文本会比吐出去的多一个字 —— **不变量 2 当场破**。
   正确顺序：`pending += piece; pump_input(); flush?(); return !cancel_requested_;`
2. **`aborted` 必须在 `!sent` 之前判。** 主动断流时 HTTP 客户端一定返回 false + `Error::Canceled`——
   先判 `!sent` 就会把"我们自己喊停"误报成传输失败（进而 RehtryableError）。而这时候 `emitted` 已经是 true，
   于是变成 `PROVIDER_ERROR`：**用户按了 Ctrl-C，看到的却是报错**。

### 决策 4：合批放在 core，默认 30ms；**`demo.chat` 不合批**

- 为什么在 core：要合的是 core → cli 这根管道上的帧，前端无从合起。
- 为什么默认开：真实模型一个长回答能吐几百个 token，每 token 一次 write+flush 是在拿 syscall 打水漂。
- 为什么 `demo.chat` 例外：`test_s2.py` 用"**一个 delta 恰好一个 UTF-8 码点**"钉着假模型，
  那是 S2 的验收基线。**新阶段的功能不该偷偷改老阶段的契约**——所以假模型保持一码点一帧。
- 首字延迟：`last_flush` 初始化为"本轮开始"，所以第一个增量通常**立刻** flush（它离"开始"往往已经 >30ms），
  后面的才攒。不需要额外为"首字"开特例。
- **S3b 的测试因此显式传了 `--delta-batch-ms=0`**：它钉的是"一个 SSE 事件 → 一帧"的解析契约，
  帧密度是 S5 新加的旋钮。**契约变了，测试跟着改并写进文档**——不是偷偷让它变绿。

### 决策 5：`turn.cancel` 的 ack 是**独立的 response**

一个请求一个响应（`protocol.md` §5 的老规矩），所以 `turn.cancel` 有它自己的 id 和 response：

```
cli ──request turn.cancel {id:"c1"} ──► core
cli ◄─response {id:"c1", ok:true, result:{cancelled:true, turn:1}} ── core   ← ack
cli ◄─response {id:"r1", result:{stop_reason:"cancelled", …}} ───────────── core   ← 本轮结算
```

两条 response 的 id 不同：前端的 `call()` 只等自己那个 id，另一个会被**忽略**（S2 就有的行为）。

**空闲时取消**（没有 turn 在跑）：`cancelled:false`、不带 `turn` 字段。
不报错是刻意的——用户按 Ctrl-C 的时机永远可能落在两轮之间，那不是错误。

---

## 3. 已知限制（刻意不做）

| 限制 | 为什么现在不修 |
|---|---|
| **只读工具并行、写工具串行**（原计划第 4 条） | 它需要真正的线程模型（或者 `fork`+非阻塞 waitpid 的调度器），和"取消"是两件事。S5 保持**单线程**，把并发单独留给 S5.1——一次只调一个未知数 |
| `--embedded` 不能取消 | 见决策 1 |
| 取消不打断正在执行的工具 | `run_shell` 有自己的看门狗（默认 30s）；要打断它得给工具层加取消通道，属于 S5.1 |
| 关闭请求（`shutdown`）在 turn 进行中要排队 | 它在 `deferred_` 里等着，本轮结束后立刻处理 |

---

## 4. 踩到的坑

1. **cli 用 stdio 的 `getline` 和 `poll(fd)` 混用 → 永久挂死。**
   `getline` 会把**后面的行**一起读进 stdio 自己的缓冲区，之后 `poll` 说"没数据"，
   而数据其实躺在 stdio 里 —— 于是永远等下去。`demo` 模式几十帧一起来，`test_s2` 立刻 30s 超时。
   修法：读 core 的 stdout 一律走**裸 fd + 自己的行缓冲**（`read_line(fd, timeout)` 返回 1/0/-1）。
   **教训：要 poll 的东西，不能让 stdio 帮你缓冲。**
2. **`on_delta` 里"先喊停再收下"会破不变量 2**（见决策 3 第 1 条）。测试里表现为
   `text` 比 delta 拼接多一个字——差一个字，足够让契约当场失效。
3. **铁律测试 grep 到了注释里的 `httplib`。** `test_s3a.py::test_layering` 是按字面搜源码的，
   我在 `core_engine.cpp` 的注释里写了"httplib 于是中止传输"——它红了。
   这不是测试太笨：**它逼着你连注释都别把 HTTP 概念往 core 里带**（改成"HTTP 客户端"就过了）。

---

## 5. 本阶段自检（能答上来才算学会）

- 为什么"读线程"不是取消的唯一解？`poll(timeout=0)` 挂在增量回调上，为什么延迟反而够低？
- 主动掐断流时，为什么必须先判 `aborted` 再判 `!sent`？判反了用户会看到什么？
- 取消为什么不做整轮回滚？"两个连续的 user" 具体坏在哪？
- 合批为什么不能顺手也用在 `demo.chat` 上？
- `turn.cancel` 空闲时为什么回 `cancelled:false` 而不是报错？

---

## 6. 验证

```bash
cd /root/project/yxAgent && cmake --build build -j8 && cd build && ctest
# 7 套全绿：S1 12 / S2 33 / S3a 20 / S3b 74 / SSE 24 / S4 51 / S5 24
```

真实模型冒烟（取消要人手动按 Ctrl-C，测试用的是假 server）：

```bash
cd /root/project/yxAgent
export DEEPSEEK_API_KEY=$(head -1 /tmp/deepseek.key.bak)
./build/agent-cli --base-url https://api.deepseek.com --model deepseek-chat
# 输入一个会长篇回答的问题，吐字过程中按 Ctrl-C → 应当看到 stop_reason=cancelled，程序继续可用
```
