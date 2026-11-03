#pragma once
// CoreEngine —— core 的协议外壳：读一帧请求，产生 0..N 个事件帧 + 恰好 1 个响应帧。
//
// 本阶段最重要的设计（详见 docs/stage-s2.md）：
//   引擎不关心自己跑在哪个进程里。run() 时帧写到 stdout；--embedded 时帧交给 cli 的回调。
//   "同进程直连"的全部秘密就是：把"写出到管道"换成"调用函数"。
//
// S5 追加（docs/stage-s5.md）：
//   * 取消要能在"一轮正在跑"的时候生效，所以读 stdin 变成**非阻塞**的（poll + read），
//     在增量回调里顺手看一眼有没有 turn.cancel。**不引入线程**——单线程同样能做到，
//     而且不用处理"谁能写 stdout"这种连锁问题。
//   * 增量合批（默认 30ms）：长回答不再一字一帧地打管道。
//
// S6 追加（docs/stage-s6.md）：
//   * 压缩只动 history_，协议只多一个 context.compacted 事件——前端仍然看不见"上下文"。
#include "common/protocol.hpp"
#include "core/context.hpp"
#include "core/provider.hpp"
#include "core/tools.hpp"

#include <cstdint>
#include <fstream>
#include <functional>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace core {

using proto::json;

// 一帧的出口（frame 已含行尾换行）
using FrameSink = std::function<void(const std::string& frame)>;

struct Options {
  std::string log_raw_path;  // 空 = 不抓包。抓包格式：每行 --> 或 <-- 加原样一帧
  int demo_delay_ms = 30;    // 假模型的"打字"速度；0 = 全速（测试与 embedded 对比用）

  // —— S3a：真模型 ——
  std::string base_url;          // 空 = 不建 provider（纯 demo 模式）
  std::string api_key;           // 只从环境变量/参数来，绝不进仓库
  std::string model;             // 空时默认 deepseek-chat
  int provider_timeout_ms = 120000;

  // —— S3b：真·流式 ——
  // true（默认）= chat 走 chat_stream()，每个增量立刻变成 assistant.delta；
  // false = 回到 S3a 的"一次性整段返回"（--no-stream；排查"是流式坏了还是模型坏了"用）。
  bool stream = true;

  // —— S4：工具与 agent 循环 ——
  // 启用的工具名（read_file / write_file / run_shell）；空 = 不给模型用工具 = S3b 的行为。
  // 刻意默认关：能改文件、能跑命令的能力不该被"顺手"打开（审批层在 S8）。
  std::vector<std::string> tools;
  std::string workdir;      // 工具的相对路径锚点，也是 run_shell 的 cwd（空 = 进程当前目录）
  int max_steps = 8;        // 一个 turn 里最多几次模型调用（ReAct 的硬上限，防死循环）

  // 系统提示词（--system）。空 = 有工具时用内置 agent 提示词，没工具时不加 system 消息。
  // 它会被塞进 history 的第 0 条：模型无状态，每次请求都要把整段 messages 重新发一遍，
  // 放在历史里才能"自动每次都带上"，也才进得了 S7 的落盘。
  std::string system_prompt;

  // —— S5：增量合批 ——
  // 同一 step 的 assistant.delta 先攒着，最多攒这么多毫秒再发一帧；0 = 不合批（一增量一帧）。
  // 合批只影响**帧的密度**，不影响不变量 2（拼接仍等于 result.text）——测试两条都验。
  int delta_batch_ms = 30;

  // —— S6：上下文预算与压缩 ——
  // 估算上限（token）。0 = 关掉整个 S6（要看原始行为时用）。
  // 默认 32000：估算是"字节/4"，中文会被低估，所以预算要留一半余量。
  size_t max_context_tokens = 32000;
  int keep_recent_turns = 3;  // 最近几轮原样保留（轮 = user + 它引发的所有消息）
};

class CoreEngine {
 public:
  explicit CoreEngine(Options opts);
  ~CoreEngine();

  // spawn 模式：stdio 上的协议循环。返回进程退出码。
  // S5 起不再接收 istream/ostream：取消需要在读 stdin 时做 poll，只有真实 fd 才能 poll。
  // （签名"诚实"比"看起来可注入"重要——之前 run(in, out) 根本没用过 in/out。）
  int run();

  // embedded 模式：处理一个请求帧（调用方已保证信封合法）。
  // 事件与响应都通过 sink 送出；sink 返回后本轮结束。
  void handle_request(const json& req, const FrameSink& sink);

  // core 的身份（握手用）
  json hello_payload() const;

  // 发出启动帧 event hello（并记入抓包）。
  // 管道模式在 run() 开头调用；embedded 模式在握手时调用——
  // 这样两种模式的握手路径与抓包内容**完全一致**，diff 抓包就等于 diff 行为。
  void greet(const FrameSink& sink);

 private:
  void log(char dir, const std::string& frame);

  // —— S5：stdin 的非阻塞读（只在 run() 里启用；embedded 绝不去抢 cli 的 stdin）——
  void pump_input();                        // 有多少读多少；解析出整行就立刻分发
  void dispatch_line(const std::string& line);  // 一行 -> 立刻处理 / 排队 / 取消
  bool read_next_line(std::string& line);   // 阻塞到凑够一整行；EOF 返回 false
  void send(const std::string& frame);      // log + 当前 sink（给 dispatch_line 用）

  Options opts_;
  std::ofstream log_;
  std::unique_ptr<Provider> provider_;  // 空 = 没有真模型（chat 会回 NO_PROVIDER）
  ToolRegistry tools_;                  // 空 = 模型没有手脚（S4 之前的行为）
  ContextManager ctx_;                  // S6：预算与压缩（只认 vector<Message>，不认识协议）
  std::vector<Message> history_;        // 消息历史（真相在 core）；S6 压缩、S7 落盘都从它来
  int64_t seq_ = 0;   // 事件序列号：从 1 开始、严格递增、无空洞（protocol.md §2.3）
  int64_t turn_ = 0;  // 第几轮对话（S2 只有 turn 这个概念，S3 会长大）
  bool shutdown_requested_ = false;
  int exit_code_ = 0;

  // —— S5 状态（单线程，全部是普通变量，不需要原子量）——
  bool stdio_mode_ = false;     // true = 正在跑 run()（可以读 stdin 找取消）；embedded 为 false
  FrameSink sink_;              // 当前帧出口。handle_request 每个请求都会重设它
  std::string in_buf_;          // 还没凑成整行的字节
  std::vector<json> deferred_;  // 一轮进行中收到的其它请求：等这轮结束再按序处理
  bool cancel_requested_ = false;
  int64_t turn_active_ = 0;     // 正在跑的 turn 号；0 = 此刻没有 turn
};

}  // namespace core
