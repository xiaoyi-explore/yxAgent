#pragma once
// CoreProcess（S9）—— TUI 的 stdio 客户端：spawn core、发请求、阻塞读帧。
// 读取线程由 TUI 自己起；本类不碰 FTXUI，也不认识 UI。
#include "common/protocol.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace core {

using proto::json;

class CoreProcess {
 public:
  CoreProcess(const std::string& core_path, const std::vector<std::string>& args);
  ~CoreProcess();

  CoreProcess(const CoreProcess&) = delete;
  CoreProcess& operator=(const CoreProcess&) = delete;

  const json& hello() const { return hello_; }
  std::string send(const std::string& method, const json& params);
  bool read_frame(json& out);  // 阻塞到一整帧；EOF 返回 false
  void close_input();
  void stop();

 private:
  void write_frame(const std::string& frame);

  std::FILE* in_ = nullptr;
  int out_fd_ = -1;
  int pid_ = -1;
  std::string rbuf_;
  int64_t next_id_ = 0;
  json hello_ = json::object();
};

}  // namespace core
