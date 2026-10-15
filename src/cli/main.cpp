// agent-cli —— 一根管子：spawn core，用户一行 = 一个 echo 请求。没有界面没有状态。
#include "common/protocol.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using proto::json;  // 协议别名进入全局作用域（S1 的两个 main 都要用）

// 从 FILE* 读一行并去行尾；EOF 返回空串
static std::string read_line(FILE* f) {
  char* buf = nullptr;
  size_t cap = 0;
  const ssize_t n = getline(&buf, &cap, f);
  if (n < 0) { free(buf); return std::string(); }
  std::string s(buf, static_cast<size_t>(n));
  free(buf);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

static void write_frame(FILE* f, const std::string& frame) {
  fputs(frame.c_str(), f);
  fflush(f);  // 铁律：写完必 flush（protocol.md §1）
}

// 读帧直到拿到 id 匹配的 response；途中的 event 一律忽略（S1 只有 hello 事件）
static json wait_response(FILE* from_core, const std::string& id) {
  while (true) {
    const std::string line = read_line(from_core);
    if (line.empty()) return json();  // EOF：core 死了
    json v = proto::parse_frame(line);
    if (v.is_null()) continue;
    const std::string type = v.contains("type") && v["type"].is_string() ? v["type"].get<std::string>() : "";
    if (type == "event") continue;
    if (type == "response" && v.contains("id") && v["id"].is_string() && v["id"].get<std::string>() == id)
      return v;
  }
}

int main(int argc, char** argv) {
  // 默认：core 与 cli 同目录
  const std::string self = argc > 0 ? argv[0] : "./agent-cli";
  const auto slash = self.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? "." : self.substr(0, slash);
  const std::string core_path = argc > 1 ? argv[1] : dir + "/agent-core";

  // spawn：两根管道（protocol.md §1：传输层 = stdio）
  int to_core[2], from_core[2];
  if (pipe(to_core) != 0 || pipe(from_core) != 0) { perror("pipe"); return 1; }
  const pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 1; }
  if (pid == 0) {
    dup2(to_core[0], STDIN_FILENO);
    dup2(from_core[1], STDOUT_FILENO);
    close(to_core[0]); close(to_core[1]);
    close(from_core[0]); close(from_core[1]);
    execl(core_path.c_str(), core_path.c_str(), static_cast<char*>(nullptr));
    perror("exec agent-core");
    _exit(127);
  }
  close(to_core[0]);
  close(from_core[1]);
  FILE* core_in  = fdopen(to_core[1], "w");
  FILE* core_out = fdopen(from_core[0], "r");

  // 握手第一步：等 core 的第一帧 event hello（protocol.md §4.1）
  const json helo = proto::parse_frame(read_line(core_out));
  if (helo.is_null() || !helo.contains("name") || helo["name"] != "hello") {
    std::cerr << "agent-cli: core did not send hello event, aborting\n";
    return 1;
  }
  std::cout << "[core ready] protocol=" << helo["payload"].value("protocol_version", "?")
            << " core=" << helo["payload"].value("core_version", "?")
            << " pid=" << helo["payload"].value("pid", 0) << "\n" << std::flush;

  // 握手第二步：request hello（版本协商，成功响应即 welcome）
  int64_t next_id = 0;
  const std::string hello_id = "r" + std::to_string(++next_id);
  write_frame(core_in, proto::frame_request(hello_id, "hello", json{
      {"protocol_version", proto::PROTOCOL_VERSION},
      {"client_version", "0.0.1"}}));
  const json welcome = wait_response(core_out, hello_id);
  if (welcome.is_null() || !welcome.value("ok", false)) {
    std::cerr << "agent-cli: handshake rejected: " << welcome.dump() << "\n";
    return 1;
  }
  std::cout << "[handshake] ok\n" << std::flush;

  // 主循环：用户一行 = 一个 echo 请求
  std::string user_line;
  while (std::getline(std::cin, user_line)) {
    const std::string rid = "r" + std::to_string(++next_id);
    write_frame(core_in, proto::frame_request(rid, "echo", json{{"line", user_line}}));
    const json resp = wait_response(core_out, rid);
    if (resp.is_null()) { std::cerr << "agent-cli: core died unexpectedly\n"; break; }
    if (resp.value("ok", false)) {
      std::cout << resp["result"].value("line", resp["result"].dump()) << "\n" << std::flush;
    } else {
      std::cout << "[error] " << resp["error"].value("code", "?") << ": "
                << resp["error"].value("message", "?") << "\n" << std::flush;
    }
  }

  // 收尾：协商关闭（protocol.md §4.2 shutdown）
  const std::string sid = "r" + std::to_string(++next_id);
  write_frame(core_in, proto::frame_request(sid, "shutdown", json::object()));
  const json bye = wait_response(core_out, sid);
  fclose(core_in);
  int status = 0;
  waitpid(pid, &status, 0);
  if (!bye.is_null() && WIFEXITED(status)) {
    std::cout << "[core exited cleanly, code=" << WEXITSTATUS(status) << "]\n";
  }
  return 0;
}
