#include "tui/core_client.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace core {

CoreProcess::CoreProcess(const std::string& core_path, const std::vector<std::string>& args) {
  std::vector<std::string> all = {core_path};
  all.insert(all.end(), args.begin(), args.end());

  int to_core[2], from_core[2];
  if (::pipe(to_core) != 0 || ::pipe(from_core) != 0)
    throw std::runtime_error("pipe() failed");
  const pid_t pid = ::fork();
  if (pid < 0) throw std::runtime_error("fork() failed");
  if (pid == 0) {
    ::dup2(to_core[0], STDIN_FILENO);
    ::dup2(from_core[1], STDOUT_FILENO);
    ::close(to_core[0]);
    ::close(to_core[1]);
    ::close(from_core[0]);
    ::close(from_core[1]);
    std::vector<char*> argv;
    for (std::string& s : all) argv.push_back(s.data());
    argv.push_back(nullptr);
    ::execv(core_path.c_str(), argv.data());
    std::perror("exec agent-core");
    ::_exit(127);
  }

  ::close(to_core[0]);
  ::close(from_core[1]);
  in_ = ::fdopen(to_core[1], "w");
  out_fd_ = from_core[0];
  pid_ = pid;
  if (in_ == nullptr || out_fd_ < 0) throw std::runtime_error("fdopen() failed");

  json first;
  if (!read_frame(first) || first.value("name", "") != "hello") {
    stop();
    throw std::runtime_error("core did not send the hello event");
  }
  hello_ = first.contains("payload") ? first["payload"] : json::object();
}

CoreProcess::~CoreProcess() { stop(); }

void CoreProcess::write_frame(const std::string& frame) {
  if (in_ == nullptr) throw std::runtime_error("core stdin is closed");
  if (std::fputs(frame.c_str(), in_) == EOF)
    throw std::runtime_error(std::string("write to core failed: ") + std::strerror(errno));
  if (std::fflush(in_) != 0)
    throw std::runtime_error(std::string("flush to core failed: ") + std::strerror(errno));
}

std::string CoreProcess::send(const std::string& method, const json& params) {
  const std::string id = "r" + std::to_string(++next_id_);
  write_frame(proto::frame_request(id, method, params));
  return id;
}

bool CoreProcess::read_frame(json& out) {
  for (;;) {
    const size_t pos = rbuf_.find('\n');
    if (pos != std::string::npos) {
      std::string line = rbuf_.substr(0, pos);
      rbuf_.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      const json f = proto::parse_frame(line);
      if (f.is_null()) continue;
      out = f;
      return true;
    }
    char buf[8192];
    const ssize_t n = ::read(out_fd_, buf, sizeof(buf));
    if (n <= 0) return false;
    rbuf_.append(buf, static_cast<size_t>(n));
  }
}

void CoreProcess::close_input() {
  if (in_ != nullptr) {
    std::fclose(in_);
    in_ = nullptr;
  }
}

void CoreProcess::stop() {
  close_input();
  if (pid_ > 0) {
    int status = 0;
    for (int i = 0; i < 50; ++i) {
      const pid_t w = ::waitpid(pid_, &status, WNOHANG);
      if (w == pid_) {
        pid_ = -1;
        return;
      }
      ::usleep(10 * 1000);
    }
    ::kill(pid_, SIGTERM);
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
  }
}

}  // namespace core