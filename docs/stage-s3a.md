# S3a 阶段设计记录 —— 接上真模型（先不流式）

> 本阶段只引入 **一个**未知数：HTTP + JSON。SSE 留给 S3b。
> 里程碑 M1：`./agent-cli` 启动后能和真模型聊天（整段返回）。

---

## 1. 交付了什么

| 层 | 内容 |
|---|---|
| 接口 | `Provider`（`core/provider.hpp`）：`ChatResult chat(const std::vector<Message>&)`，**不含任何 HTTP 类型** |
| 实现 | `OpenAICompatProvider`：`POST {base}/chat/completions`，`stream:false` |
| 协议 | 新方法 `chat`（protocol.md §9 S3a） |
| 历史 | `messages` 数组进 core；失败轮次回滚 |
| 配置 | `--base-url` / `--model` / `--api-key` / `--provider-timeout-ms`；key 优先环境变量 |
| 测试 | `tests/fake_llm.py`（六种行为）+ `tests/test_s3a.py`（13 个断言组） |

---

## 2. 关键决策

### 决策 1：先非流式，再流式

流式 = HTTP + 分片边界 + 编码三重难度叠加。若一次上，出问题时你分不清是"HTTP 没通"还是"解析崩了"。
S3a 只让 HTTP 通、只让 JSON 对、只让历史正确——**这三件事全绿了，S3b 才有资格谈"边界"**。

### 决策 2：Provider 为什么放在 core

前端不该知道 HTTP 的存在（铁律）。把 Provider 放 core 还有两个远期收益：

1. **换模型 = 换一个 `Provider` 实现**，前端零改动（S3b 换流式、以后接本地 vLLM 都是这样）；
2. **密钥只存在 core 的进程里**，cli 的命令行和日志里永远不会出现 key。

### 决策 3：错误一律 `PROVIDER_ERROR`，且失败轮次**回滚历史**

`Provider::chat` 抛异常（`ProviderError`），引擎统一捕获成错误响应。这样"失败"只有一条出路，
不会出现"有时抛、有时返回空字符串"的双重语义。

回滚历史的原因值得单独记：如果失败的那句留在 `messages` 里，下一次请求就会带着**半截对话**
（user 有、assistant 没有）发给模型——模型会困惑，而你在日志里看不出原因。测试用 `/failonce`
验证了这一点：失败后第二轮只发 1 条消息。

### 决策 4：密钥只走环境变量（`DEEPSEEK_API_KEY`），`--api-key` 只是兜底

命令行参数会出现在 `ps aux` / `/proc/<pid>/cmdline` 里，**同机任何用户都能看到**。
所以：cli 不提供 `--api-key`，key 由环境变量继承给 core；core 保留 `--api-key` 兜底（测试/临时用）。

### 决策 5：分层铁律的执行方式变了——从"自觉"变成"测试"

S2 时我们说"cli 源码不含 HTTP"，靠嘴说。S3a 起 provider 就在 `agent_core_lib` 里，
链接层面 cli 必然拿到 httplib。于是把铁律降级为**源码级**并写进测试：

```
tests/test_s3a.py::test_layering   # 扫 src/cli/**，出现 httplib / openssl / ftxui 就 FAIL
```

这是"规则要有执行者"的例子：**不能执行的规则，迟早会烂**。

---

## 3. 踩坑记录

1. **base_url 的形状有三种**：`https://api.deepseek.com`、`.../v1`、`http://127.0.0.1:18080`。
   拼接逻辑若写死 `base + "/chat/completions"`，第一种对、第二种错。修法：先拆成
   `(origin, path-prefix)`，再拼 `/chat/completions`——`parse_endpoint()` 里 12 行办完。
2. **连接超时与读超时不是一回事**：连不上要**快失败**（10 秒），模型思考要**慢慢等**（120 秒）。
   httplib 的 `set_connection_timeout` / `set_read_timeout` 必须分开设，测试里用
   `--provider-timeout-ms=600` 打 `/slow` 验证了"超时 0.6 秒内报错，而不是傻等 3 秒"。

---

## 4. 已知限制（刻意不修）

| 限制 | 谁来修 |
|---|---|
| 没有重试（401 之外的偶发 5xx 直接失败） | S3b（与流式断连一起做） |
| 历史无上限，token 会一直涨 | S6（ContextManager） |
| 不能用 Esc 中断一个正在进行的请求 | S5（取消语义） |
| 非流式：用户要等整段生成完才看到字 | S3b |

---

## 5. 自检（答不上来就回读）

- 为什么 `Provider` 接口里看不到 `httplib` 的任何类型？
- 失败的一轮如果不回滚历史，第二次请求会发出几条消息、模型会看到什么？
- `--api-key` 为什么只是兜底？它在哪能看到？
