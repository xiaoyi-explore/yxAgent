#pragma once
// CoreEngine —— core 的协议外壳：读一帧请求，产生 0..N 个事件帧 + 恰好 1 个响应帧。
//
// 本阶段最重要的设计（详见 docs/stage-s2.md）：
//   引擎不关心自己跑在哪个进程里。run() 时帧写到 stdout；--embedded 时帧交给 cli 的回调。
//   "同进程直连"的全部秘密就是：把"写出到管道"换成"调用函数"。
#include "common/protocol.hpp"

#include <cstdint>
#include <fstream>
#include <functional>
#include <istream>
#include <ostream>
#include <string>

namespace core {

using proto::json;

// 一帧的出口（frame 已含行尾换行）
using FrameSink = std::function<void(const std::string& frame)>;

struct Options {
  std::string log_raw_path;  // 空 = 不抓包。抓包格式：每行 --> 或 <-- 加原样一帧
  int demo_delay_ms = 30;    // 假模型的"打字"速度；0 = 全速（测试与 embedded 对比用）
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

  // core 的身份（握手用）：管道模式从 event hello 里读，embedded 模式直接问引擎
  json hello_payload() const;

 private:
  void log(char dir, const std::string& frame);

  Options opts_;
  std::ofstream log_;
  int64_t seq_ = 0;   // 事件序列号：从 1 开始、严格递增、无空洞（protocol.md §2.3）
  int64_t turn_ = 0;  // 第几轮对话（S2 只有 turn 这个概念，S3 会长大）
  bool shutdown_requested_ = false;
  int exit_code_ = 0;
};

}  // namespace core
