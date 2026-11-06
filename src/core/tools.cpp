#include "core/tools.hpp"

#include "common/utf8.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

namespace core {
namespace {

constexpr size_t kReadDefaultBytes = 200 * 1024;   // 单次读文件上限
constexpr size_t kShellOutputCap = 64 * 1024;      // 单次命令输出上限（喂回模型的文本）
constexpr int kShellTimeoutDefaultMs = 30000;
constexpr int kShellTimeoutMaxMs = 120000;

// 相对路径锚在 workdir 上；绝对路径原样用。
// S4 刻意**不做**路径沙箱：那需要一次"问用户"的交互，属于 S8 的审批层。
std::string join_path(const std::string& workdir, const std::string& p) {
  if (p.empty()) return p;
  if (p[0] == '/') return p;
  if (workdir.empty()) return p;
  return workdir + "/" + p;
}

bool is_directory(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  return S_ISDIR(st.st_mode);
}

// ---- read_file ----
class ReadFileTool : public Tool {
 public:
  explicit ReadFileTool(std::string workdir) : workdir_(std::move(workdir)) {}

  std::string name() const override { return "read_file"; }
  std::string description() const override {
    return "Read a UTF-8 text file. Relative paths resolve against the agent working directory. "
           "Large files are truncated at max_bytes (default 204800).";
  }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"path", {{"type", "string"}, {"description", "Path of the file to read"}}},
                  {"max_bytes",
                   {{"type", "integer"},
                    {"description", "Read at most this many bytes (default 204800)"}}}}},
                {"required", json::array({"path"})}};
  }

  ToolResult run(const json& args) const override {
    if (!args.contains("path") || !args["path"].is_string() || args["path"].get<std::string>().empty())
      return ToolResult::failure("missing required argument: path");

    const std::string path = join_path(workdir_, args["path"].get<std::string>());
    if (is_directory(path)) return ToolResult::failure("is a directory, not a file: " + path);

    size_t max_bytes = kReadDefaultBytes;
    if (args.contains("max_bytes") && args["max_bytes"].is_number_integer()) {
      const long long v = args["max_bytes"].get<long long>();
      if (v > 0) max_bytes = static_cast<size_t>(v);
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) return ToolResult::failure("cannot open file: " + path);

    f.seekg(0, std::ios::end);
    const std::streamoff total = f.tellg();
    f.seekg(0, std::ios::beg);

    std::string data;
    data.resize(max_bytes);
    f.read(&data[0], static_cast<std::streamsize>(max_bytes));
    data.resize(static_cast<size_t>(f.gcount()));
    // max_bytes 可能正好切在 UTF-8 码点中间；JSON 序列化前先退到完整边界。
    data.resize(proto::utf8_prefix_bytes(data, data.size()));

    if (total > static_cast<std::streamoff>(data.size())) {
      data += "\n[truncated: read " + std::to_string(data.size()) + " of " + std::to_string(total) +
              " bytes; call again with a larger max_bytes or read a smaller range]";
    }
    return ToolResult::success(std::move(data));
  }

 private:
  std::string workdir_;
};

// ---- write_file ----
class WriteFileTool : public Tool {
 public:
  explicit WriteFileTool(std::string workdir) : workdir_(std::move(workdir)) {}

  std::string name() const override { return "write_file"; }
  std::string description() const override {
    return "Create or overwrite a UTF-8 text file. The parent directory must already exist. "
           "Returns how many bytes were written.";
  }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"path", {{"type", "string"}, {"description", "Path of the file to write"}}},
                  {"content", {{"type", "string"}, {"description", "Full new content"}}}}},
                {"required", json::array({"path", "content"})}};
  }

  ToolResult run(const json& args) const override {
    if (!args.contains("path") || !args["path"].is_string() || args["path"].get<std::string>().empty())
      return ToolResult::failure("missing required argument: path");
    if (!args.contains("content") || !args["content"].is_string())
      return ToolResult::failure("missing required argument: content");

    const std::string path = join_path(workdir_, args["path"].get<std::string>());
    if (is_directory(path)) return ToolResult::failure("is a directory, not a file: " + path);

    const std::string content = args["content"].get<std::string>();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return ToolResult::failure("cannot open for writing (does the parent directory exist?): " + path);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f) return ToolResult::failure("write failed: " + path);
    return ToolResult::success("wrote " + std::to_string(content.size()) + " bytes to " + path);
  }

 private:
  std::string workdir_;
};

// 把管道里已经就绪的字节读干净（非阻塞读；0 = EOF，-1 = 暂时没数据）
void drain(int fd, std::string& out, size_t cap, size_t& total) {
  char buf[8192];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) return;
    total += static_cast<size_t>(n);
    if (out.size() < cap) {
      const size_t room = cap - out.size();
      out.append(buf, static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room);
    }
  }
}

// ---- run_shell ----
// fork + exec /bin/sh -c；stdout/stderr 合并到一个管道。
// 为什么要自己做超时：一个挂死的命令会挂死整个 turn，而 turn 是同步的——
// 没有看门狗的话，模型只要写一句 "sleep 1000" 就把 agent 冻住了。
class RunShellTool : public Tool {
 public:
  explicit RunShellTool(std::string workdir) : workdir_(std::move(workdir)) {}

  std::string name() const override { return "run_shell"; }
  std::string description() const override {
    return "Run a shell command with /bin/sh -c in the agent working directory. "
           "stdout and stderr are merged. Returns exit_code plus the output. "
           "Default timeout 30s (max 120s); the process is killed when it expires.";
  }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"command", {{"type", "string"}, {"description", "Shell command to run"}}},
                  {"timeout_ms",
                   {{"type", "integer"},
                    {"description", "Kill the command after this many ms (default 30000, max 120000)"}}}}},
                {"required", json::array({"command"})}};
  }

  ToolResult run(const json& args) const override {
    if (!args.contains("command") || !args["command"].is_string() || args["command"].get<std::string>().empty())
      return ToolResult::failure("missing required argument: command");

    const std::string command = args["command"].get<std::string>();
    int timeout_ms = kShellTimeoutDefaultMs;
    if (args.contains("timeout_ms") && args["timeout_ms"].is_number_integer()) {
      timeout_ms = static_cast<int>(args["timeout_ms"].get<long long>());
      if (timeout_ms <= 0) timeout_ms = kShellTimeoutDefaultMs;
      if (timeout_ms > kShellTimeoutMaxMs) timeout_ms = kShellTimeoutMaxMs;
    }

    int fds[2];
    if (::pipe(fds) != 0) return ToolResult::failure(std::string("pipe() failed: ") + std::strerror(errno));

    const pid_t pid = ::fork();
    if (pid < 0) {
      ::close(fds[0]);
      ::close(fds[1]);
      return ToolResult::failure(std::string("fork() failed: ") + std::strerror(errno));
    }
    if (pid == 0) {  // 子进程：只留 stdout/stderr -> 管道
      ::dup2(fds[1], STDOUT_FILENO);
      ::dup2(fds[1], STDERR_FILENO);
      ::close(fds[0]);
      ::close(fds[1]);
      if (!workdir_.empty()) {
        if (::chdir(workdir_.c_str()) != 0) ::_exit(127);
      }
      ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);
    }

    ::close(fds[1]);
    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

    std::string out;
    size_t total = 0;
    bool timed_out = false;
    int status = 0;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      drain(fds[0], out, kShellOutputCap, total);
      const pid_t w = ::waitpid(pid, &status, WNOHANG);
      if (w == pid) break;  // 正常结束
      if (w < 0 && errno != EINTR) break;
      if (std::chrono::steady_clock::now() >= deadline) {
        timed_out = true;
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    drain(fds[0], out, kShellOutputCap, total);  // 收尾：子进程退出后管道里还可能剩数据
    ::close(fds[0]);

    int exit_code = -1;
    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);

    if (total > out.size()) {
      out += "\n[truncated: " + std::to_string(total) + " bytes of output, showing the first " +
             std::to_string(out.size()) + "]";
    }

    out.resize(proto::utf8_prefix_bytes(out, out.size()));
    const std::string body = "exit_code=" + std::to_string(exit_code) + "\n" + out;
    if (timed_out)
      return ToolResult{false, body, "timeout after " + std::to_string(timeout_ms) + "ms (killed)"};
    if (exit_code != 0)
      return ToolResult{false, body, "exit code " + std::to_string(exit_code)};
    return ToolResult::success(body);
  }

 private:
  std::string workdir_;
};

}  // namespace

void ToolRegistry::add(std::unique_ptr<Tool> tool) {
  if (tool) tools_.push_back(std::move(tool));
}

std::string ToolRegistry::names() const {
  std::string s;
  for (const auto& t : tools_) {
    if (!s.empty()) s += ",";
    s += t->name();
  }
  return s;
}

std::vector<ToolSpec> ToolRegistry::specs() const {
  std::vector<ToolSpec> out;
  out.reserve(tools_.size());
  for (const auto& t : tools_) out.push_back(t->spec());
  return out;
}

const Tool* ToolRegistry::find(const std::string& name) const {
  for (const auto& t : tools_)
    if (t->name() == name) return t.get();
  return nullptr;
}

ToolResult ToolRegistry::execute(const std::string& name, const json& args) {
  const Tool* t = find(name);
  if (t == nullptr) return ToolResult::failure("unknown tool: " + name);
  try {
    return t->run(args);
  } catch (const std::exception& e) {
    return ToolResult::failure(std::string("tool threw an exception: ") + e.what());
  } catch (...) {
    return ToolResult::failure("tool threw an unknown exception");
  }
}

std::unique_ptr<Tool> make_read_file_tool(std::string workdir) {
  return std::unique_ptr<Tool>(new ReadFileTool(std::move(workdir)));
}

std::unique_ptr<Tool> make_write_file_tool(std::string workdir) {
  return std::unique_ptr<Tool>(new WriteFileTool(std::move(workdir)));
}

std::unique_ptr<Tool> make_run_shell_tool(std::string workdir) {
  return std::unique_ptr<Tool>(new RunShellTool(std::move(workdir)));
}

std::vector<std::string> parse_tool_list(const std::string& value) {
  std::vector<std::string> out;
  if (value.empty() || value == "none") return out;
  if (value == "all") return {"read_file", "write_file", "run_shell"};
  std::string cur;
  for (const char c : value) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (c != ' ') {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::unique_ptr<Tool> make_builtin_tool(const std::string& name, const std::string& workdir) {
  if (name == "read_file") return make_read_file_tool(workdir);
  if (name == "write_file") return make_write_file_tool(workdir);
  if (name == "run_shell") return make_run_shell_tool(workdir);
  return nullptr;
}

}  // namespace core