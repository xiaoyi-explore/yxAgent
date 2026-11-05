// agent-run —— S7 的一次性 headless 前端。
//
// 它只做一件事：spawn agent-core → 握手 → chat → 把最终答复打到 stdout → shutdown。
// **不 include 任何 core 内部头、不链接 agent_core_lib**：能跑起来就证明"同一 core，
// 两个前端"没有偷偷依赖 --embedded。工具进度/压缩/错误都走 stderr，stdout 只有答案。
#include "common/protocol.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

using proto::json;

namespace {

struct RunArgs {
  std::string core_path;
  std::string base_url;
  std::string model;
  std::string tools;
  std::string workdir;
  std::string system_prompt;
  std::string session_id;
  std::string resume_id;
  bool resume_latest = false;
  std::string session_dir;
  std::string approval = "never";  // headless 默认：危险操作直接拒绝，不等人
  int approval_timeout_ms = 120000;
  int max_steps = 8;
  int max_context_tokens = 32000;
  int keep_recent_turns = 3;
  int provider_timeout_ms = 120000;
  int demo_delay_ms = 0;
  bool stream = true;
  std::string prompt;
};

void usage() {
  std::cerr
      << "usage: agent-run [options] \"<prompt>\"\n"
         "  --core PATH         agent-core 的路径（默认与本程序同目录）\n"
         "  --base-url URL      模型端点（默认 https://api.deepseek.com）\n"
         "  --model NAME        模型名（默认 deepseek-chat）\n"
         "  --tools LIST        all 或逗号分隔（read_file,write_file,run_shell），默认不给\n"
         "  --workdir DIR       工具的相对路径锚点 / run_shell 的工作目录\n"
         "  --system TEXT       系统提示词\n"
         "  --session ID        开/建会话（~/.agent/sessions/<id>.jsonl）\n"
         "  --resume[=ID]       恢复最近的会话，或指定已存在的 ID\n"
         "  --session-dir DIR   会话目录（默认 ~/.agent/sessions）\n"
         "  --approval MODE     权限策略：never（默认）/ ask / allow\n"
         "  --approval-timeout-ms N  ask 等回答的上限（默认 120000）\n"
         "  --max-steps N       一个 turn 里最多几次模型调用（默认 8）\n"
         "  --max-context-tokens N  上下文预算（默认 32000，0 = 不压缩）\n"
         "  --keep-recent-turns N   压缩时最近几轮原样保留（默认 3）\n"
         "  --no-stream         透传给 core：不用 SSE\n"
         "  API key 从环境变量 DEEPSEEK_API_KEY 读取（不会出现在命令行里）\n";
}

bool opt_value(int argc, char** argv, int& i, const char* key, std::string& out) {
  const std::string a = argv[i];
  const std::string k = key;
  if (a == k) {
    if (i + 1 >= argc) return false;
    out = argv[++i];
    return true;
  }
  if (a.rfind(k + "=", 0) == 0) {
    out = a.substr(k.size() + 1);
    return true;
  }
  return false;
}

std::string default_core_path(const char* argv0) {
  const std::string self = (argv0 != nullptr) ? argv0 : "./agent-run";
  const auto slash = self.find_last_of('/');
  return (slash == std::string::npos ? "." : self.substr(0, slash)) + "/agent-core";
}

// ---------------------------------------------------------------- 最小协议客户端
// 刻意不依赖 cli 的 PipeSession：agent-run 要能被单独审查"只靠协议"。
class CoreProcess {
 public:
  CoreProcess(const std::string& core_path, const std::vector<std::string>& args) {
    std::vector<std::string> all = {core_path};
    all.insert(all.end(), args.begin(), args.end());

    int to_core[2], from_core[2];
    if (::pipe(to_core) != 0 || ::pipe(from_core) != 0) throw std::runtime_error("pipe() failed");
    pid_ = ::fork();
    if (pid_ < 0) throw std::runtime_error("fork() failed");
    if (pid_ == 0) {
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
    if (in_ == nullptr || out_fd_ < 0) throw std::runtime_error("fdopen() failed");

    std::string line;
    const json helo = (read_line(line) == 1) ? proto::parse_frame(line) : json();
    if (helo.is_null() || helo.value("name", "") != "hello")
      throw std::runtime_error("core did not send the hello event");
    hello_ = helo.contains("payload") ? helo["payload"] : json::object();
  }

  ~CoreProcess() {
    if (in_ != nullptr) std::fclose(in_);
    if (out_fd_ >= 0) ::close(out_fd_);
    if (pid_ > 0) {
      int st = 0;
      ::waitpid(pid_, &st, 0);
    }
  }

  const json& hello() const { return hello_; }

  json call(const std::string& method, const json& params,
            const std::function<void(const json&)>& on_event) {
    const std::string rid = "r" + std::to_string(++next_id_);
    write_frame(proto::frame_request(rid, method, params));
    for (;;) {
      std::string line;
      const int st = read_line(line);
      if (st < 0) return json();
      const json v = proto::parse_frame(line);
      if (v.is_null()) continue;
      const std::string type = v.value("type", "");
      if (type == "event") {
        if (on_event) on_event(v);
        continue;
      }
      if (type == "response" && v.contains("id") && v["id"].is_string() &&
          v["id"].get<std::string>() == rid)
        return v;
    }
  }

  // headless 没有 stdin 可问：approval.requested 一律拒绝，并且立刻把回答发回去。
  void respond_approval(const json& payload, const std::string& decision) {
    const std::string aid = payload.value("approval_id", "");
    const std::string rid = "a" + std::to_string(++next_id_);
    write_frame(proto::frame_request(rid, "approval.respond",
                                     json{{"approval_id", aid}, {"decision", decision}}));
  }

  int shutdown() {
    const json bye = call("shutdown", json::object(), nullptr);
    (void)bye;
    if (in_ != nullptr) {
      std::fclose(in_);
      in_ = nullptr;
    }
    int status = 0;
    if (pid_ > 0) ::waitpid(pid_, &status, 0);
    const int code = (pid_ > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    pid_ = -1;
    return code;
  }

 private:
  int read_line(std::string& line) {
    for (;;) {
      const size_t pos = rbuf_.find('\n');
      if (pos != std::string::npos) {
        line = rbuf_.substr(0, pos);
        rbuf_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return 1;
      }
      char chunk[8192];
      const ssize_t n = ::read(out_fd_, chunk, sizeof(chunk));
      if (n <= 0) return -1;
      rbuf_.append(chunk, static_cast<size_t>(n));
    }
  }

  void write_frame(const std::string& frame) {
    if (in_ == nullptr) throw std::runtime_error("core stdin is closed");
    std::fputs(frame.c_str(), in_);
    std::fflush(in_);
  }

  pid_t pid_ = -1;
  std::FILE* in_ = nullptr;
  int out_fd_ = -1;
  std::string rbuf_;
  int64_t next_id_ = 0;
  json hello_;
};

std::vector<std::string> core_args(const RunArgs& a) {
  std::vector<std::string> args;
  args.push_back("--provider-timeout-ms=" + std::to_string(a.provider_timeout_ms));
  args.push_back("--demo-delay-ms=" + std::to_string(a.demo_delay_ms));
  args.push_back(a.stream ? "--stream=true" : "--stream=false");
  if (!a.base_url.empty()) args.push_back("--base-url=" + a.base_url);
  if (!a.model.empty()) args.push_back("--model=" + a.model);
  if (!a.tools.empty()) args.push_back("--tools=" + a.tools);
  if (!a.workdir.empty()) args.push_back("--workdir=" + a.workdir);
  if (!a.system_prompt.empty()) args.push_back("--system=" + a.system_prompt);
  args.push_back("--max-steps=" + std::to_string(a.max_steps));
  args.push_back("--max-context-tokens=" + std::to_string(a.max_context_tokens));
  args.push_back("--keep-recent-turns=" + std::to_string(a.keep_recent_turns));
  if (!a.session_id.empty()) args.push_back("--session=" + a.session_id);
  if (!a.resume_id.empty()) args.push_back("--resume=" + a.resume_id);
  else if (a.resume_latest) args.push_back("--resume");
  if (!a.session_dir.empty()) args.push_back("--session-dir=" + a.session_dir);
  args.push_back("--approval=" + a.approval);
  args.push_back("--approval-timeout-ms=" + std::to_string(a.approval_timeout_ms));
  return args;
}

int run(const RunArgs& a) {
  CoreProcess core(a.core_path, core_args(a));

  const json welcome = core.call(
      "hello", json{{"protocol_version", proto::PROTOCOL_VERSION}, {"client_version", "agent-run"}},
      nullptr);
  if (welcome.is_null() || !welcome.value("ok", false)) {
    std::cerr << "agent-run: handshake rejected: " << welcome.dump() << "\n";
    return 1;
  }
  if (core.hello().contains("session") && core.hello()["session"].is_object()) {
    const json& s = core.hello()["session"];
    std::cerr << "[session] " << (s.value("resumed", false) ? "resumed" : "new") << " "
              << s.value("id", "?") << " (turns=" << s.value("turns", 0) << ")\n";
  }

  const json resp = core.call("chat", json{{"text", a.prompt}}, [&core](const json& ev) {
    const std::string name = ev.value("name", "");
    const json payload = ev.contains("payload") ? ev["payload"] : json::object();
    if (name == "approval.requested") {
      std::cerr << "[approval] " << payload.value("name", "?") << " "
                << payload.value("arguments", json::object()).dump()
                << " -> deny (headless; pass --approval=allow to opt in)\n";
      core.respond_approval(payload, "deny");
    } else if (name == "tool.call") {
      std::cerr << "[tool] " << payload.value("name", "?") << " "
                << payload.value("arguments", json::object()).dump() << "\n";
    } else if (name == "tool.result") {
      std::cerr << "[tool] -> " << (payload.value("ok", false) ? "ok" : "error") << "\n";
    } else if (name == "context.compacted") {
      std::cerr << "[context] compacted ~" << payload.value("before_tokens", 0) << " -> ~"
                << payload.value("after_tokens", 0) << " tokens\n";
    }
  });

  if (resp.is_null()) {
    std::cerr << "agent-run: core died unexpectedly\n";
    return 3;
  }
  if (!resp.value("ok", false)) {
    std::cerr << "agent-run: " << resp["error"].value("code", "?") << ": "
              << resp["error"].value("message", "?") << "\n";
    return 1;
  }
  const std::string text = resp["result"].value("text", "");
  std::cout << text;
  if (!text.empty() && text.back() != '\n') std::cout << "\n";
  std::cout << std::flush;

  const int code = core.shutdown();
  if (code != 0) {
    std::cerr << "agent-run: core exited with code " << code << "\n";
    return 3;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  RunArgs a;
  bool core_path_set = false;
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    std::string v;
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else if (arg == "--embedded") {
      std::cerr << "agent-run: --embedded is deliberately not supported; this front-end speaks only the protocol\n";
      return 1;
    } else if (arg == "--no-stream") a.stream = false;
    else if (arg == "--stream") a.stream = true;
    else if (arg == "--resume") a.resume_latest = true;
    else if (arg.rfind("--resume=", 0) == 0) a.resume_id = arg.substr(9);
    else if (opt_value(argc, argv, i, "--core", v)) { a.core_path = v; core_path_set = true; }
    else if (opt_value(argc, argv, i, "--base-url", v)) a.base_url = v;
    else if (opt_value(argc, argv, i, "--model", v)) a.model = v;
    else if (opt_value(argc, argv, i, "--tools", v)) a.tools = v;
    else if (opt_value(argc, argv, i, "--workdir", v)) a.workdir = v;
    else if (opt_value(argc, argv, i, "--system", v)) a.system_prompt = v;
    else if (opt_value(argc, argv, i, "--session", v)) a.session_id = v;
    else if (opt_value(argc, argv, i, "--session-dir", v)) a.session_dir = v;
    else if (opt_value(argc, argv, i, "--approval", v)) a.approval = v;
    else if (opt_value(argc, argv, i, "--approval-timeout-ms", v))
      a.approval_timeout_ms = std::max(0, std::atoi(v.c_str()));
    else if (opt_value(argc, argv, i, "--max-steps", v)) a.max_steps = std::max(1, std::atoi(v.c_str()));
    else if (opt_value(argc, argv, i, "--max-context-tokens", v))
      a.max_context_tokens = std::max(0, std::atoi(v.c_str()));
    else if (opt_value(argc, argv, i, "--keep-recent-turns", v))
      a.keep_recent_turns = std::max(1, std::atoi(v.c_str()));
    else if (opt_value(argc, argv, i, "--provider-timeout-ms", v))
      a.provider_timeout_ms = std::max(1, std::atoi(v.c_str()));
    else if (opt_value(argc, argv, i, "--demo-delay-ms", v))
      a.demo_delay_ms = std::max(0, std::atoi(v.c_str()));
    else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "agent-run: unknown option: " << arg << "\n";
      usage();
      return 1;
    } else {
      positional.push_back(arg);
    }
  }

  if (a.core_path.empty())
    a.core_path = default_core_path(argc > 0 ? argv[0] : nullptr);
  if (!a.session_id.empty() && (a.resume_latest || !a.resume_id.empty())) {
    std::cerr << "agent-run: --session and --resume are mutually exclusive\n";
    return 1;
  }
  if (a.approval != "ask" && a.approval != "never" && a.approval != "allow" &&
      a.approval != "deny" && a.approval != "auto") {
    std::cerr << "agent-run: --approval must be ask, never or allow (got '" << a.approval << "')\n";
    return 1;
  }
  if (positional.empty()) {
    usage();
    return 1;
  }
  for (const std::string& p : positional) {
    if (!a.prompt.empty()) a.prompt += " ";
    a.prompt += p;
  }

  try {
    return run(a);
  } catch (const std::exception& e) {
    std::cerr << "agent-run: " << e.what() << "\n";
    return 1;
  }
}