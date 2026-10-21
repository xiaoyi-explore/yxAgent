#pragma once
// CoreEngine —— core 的协议外壳：读一帧请求，产生 0..N 个事件帧 + 恰好 1 个响应帧。
//
// 本阶段最重要的设计（详见 docs/stage-s2.md）：
//   引擎不关心自己跑在哪个进程里。run() 时帧写到 stdout；--embedded 时帧交给 cli 的回调。
//   "同进程直连"的全部秘密就是：把"写出到管道"换成"调用函数"。
#include "common/protocol.hpp"
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

  // 系统提示词（--system）。空 = 有工具时用内置的 agent 提示词，没工具时不加 system 消息。
  // 它会被塞进 history 的第 0 条：模型无状态，每次请求都要把整段 messages 重新发一遍，
  // 放在历史里才能"自动每次都带上"，也才进得了 S7 的落盘。
  std::string system_prompt;
};

class CoreEngine {
 public:
  explicit CoreEngine(Options opts);
  ~CoreEngine();

  // spawn 模式：stdio 上的协议循环。返回进程退出码。
  int run(std::istream& in, std::ostream& out);

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

  Options opts_;
  std::ofstream log_;
  std::unique_ptr<Provider> provider_;  // 空 = 没有真模型（chat 会回 NO_PROVIDER）
  ToolRegistry tools_;                  // 空 = 模型没有手脚（S4 之前的行为）
  std::vector<Message> history_;        // 消息历史（真相在 core）；S6 压缩、S7 落盘都从它来
  int64_t seq_ = 0;   // 事件序列号：从 1 开始、严格递增、无空洞（protocol.md §2.3）
  int64_t turn_ = 0;  // 第几轮对话（S2 只有 turn 这个概念，S3 会长大）
  bool shutdown_requested_ = false;
  int exit_code_ = 0;
};

}  // namespace core
