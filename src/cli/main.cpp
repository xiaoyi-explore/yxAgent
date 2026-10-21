// agent-cli —— 前端。S2 起它做三件事：
//   1) 连上 core：默认 spawn 子进程（管道），--embedded 时同进程直连（回调）
//   2) 握手（protocol.md §4.1）
//   3) REPL：把用户每行包装成 demo.chat，实时渲染 assistant.delta（打字机效果）
//
// 它不认识 HTTP / 工具 / 上下文——那些都在 core 里。前端只消费事件。
#include "common/protocol.hpp"
#include "core/core_engine.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

using proto::json;

namespace {

using EventSink = std::function<void(const json& ev)>;

// ============================ 与 core 的连接 ============================
// 两种实现，对上层是同一件事：call() 发一个请求，途中事件实时回调，最后拿到响应。
// 这正是双进程架构的"接缝"：换传输不改上层逻辑（S7 还会加一个 unix socket 实现）。
class Session {
 public:
  virtual ~Session() = default;
  json hello;  // core 的身份（握手第一步收到的 payload）
  // 返回最终响应；null json 表示连接断了
  virtual json call(const std::string& method, const json& params, const EventSink& on_event) = 0;
  virtual int shutdown() = 0;  // 返回 core 的退出码
};

// ---------- 实现一：spawn 子进程 + 两根管道（默认，真双进程）----------
class PipeSession : public Session {
 public:
  PipeSession(const std::string& core_path, const core::Options& opts) {
    std::vector<std::string> args = {core_path};
    if (!opts.log_raw_path.empty()) args.push_back("--log-raw=" + opts.log_raw_path);
    args.push_back("--demo-delay-ms=" + std::to_string(opts.demo_delay_ms));
    if (!opts.base_url.empty()) args.push_back("--base-url=" + opts.base_url);
    if (!opts.model.empty()) args.push_back("--model=" + opts.model);
    args.push_back("--provider-timeout-ms=" + std::to_string(opts.provider_timeout_ms));
    args.push_back(opts.stream ? "--stream=true" : "--stream=false");  // S3b：流式与否由 core 决定
    if (!opts.tools.empty()) {
      std::string list;
      for (const std::string& t : opts.tools) {
        if (!list.empty()) list += ",";
        list += t;
      }
      args.push_back("--tools=" + list);                                 // S4：工具清单透传给 core
    }
    if (!opts.workdir.empty()) args.push_back("--workdir=" + opts.workdir);
    args.push_back("--max-steps=" + std::to_string(opts.max_steps));

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
      for (std::string& s : args) argv.push_back(s.data());
      argv.push_back(nullptr);
      ::execv(core_path.c_str(), argv.data());
      std::perror("exec agent-core");
      ::_exit(127);
    }
    ::close(to_core[0]);
    ::close(from_core[1]);
    in_ = ::fdopen(to_core[1], "w");
    out_ = ::fdopen(from_core[0], "r");
    if (in_ == nullptr || out_ == nullptr) throw std::runtime_error("fdopen() failed");

    // 握手第一步：第一帧永远是 event hello（protocol.md §4.1）
    const json helo = proto::parse_frame(read_line(out_));
    if (helo.is_null() || helo.value("name", "") != "hello")
      throw std::runtime_error("core did not send the hello event");
    hello = helo.contains("payload") ? helo["payload"] : json::object();
  }

  ~PipeSession() override {
    if (in_ != nullptr) std::fclose(in_);
    if (out_ != nullptr) std::fclose(out_);
    if (pid_ > 0) {
      int st = 0;
      ::waitpid(pid_, &st, 0);
    }
  }

  json call(const std::string& method, const json& params, const EventSink& on_event) override {
    const std::string rid = "r" + std::to_string(++next_id_);
    write_frame(in_, proto::frame_request(rid, method, params));
    while (true) {
      const std::string line = read_line(out_);
      if (line.empty()) return json();  // EOF：core 死了
      const json v = proto::parse_frame(line);
      if (v.is_null()) continue;
      const std::string type = v.value("type", "");
      if (type == "event") { on_event(v); continue; }
      if (type == "response" && v.contains("id") && v["id"].is_string() && v["id"].get<std::string>() == rid)
        return v;
      // 其他 response（id 不匹配）不属于本次调用，忽略
    }
  }

  int shutdown() override {
    const json bye = call("shutdown", json::object(), [](const json&) {});
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
  static std::string read_line(FILE* f) {
    char* buf = nullptr;
    size_t cap = 0;
    const ssize_t n = ::getline(&buf, &cap, f);
    if (n < 0) { std::free(buf); return std::string(); }
    std::string s(buf, static_cast<size_t>(n));
    std::free(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
  }
  static void write_frame(FILE* f, const std::string& frame) {
    std::fputs(frame.c_str(), f);
    std::fflush(f);  // 铁律：写完必 flush（protocol.md §1）
  }

  pid_t pid_ = -1;
  FILE* in_ = nullptr;
  FILE* out_ = nullptr;
  int64_t next_id_ = 0;
};

// ---------- 实现二：同进程直连（--embedded）----------
// 没有 fork、没有管道、没有线程：事件就是一次函数回调。
// 价值是调试（一次只调一个未知数）；代价是 cli 链接了 core 的静态库（见 docs/stage-s2.md）。
class EmbeddedSession : public Session {
 public:
  explicit EmbeddedSession(core::Options opts) : engine_(std::move(opts)) {
    // 握手第一步与管道模式走同一条路：解析 core 发出的 event hello（只换传输，不换语义）
    engine_.greet([&](const std::string& frame) {
      const json ev = proto::parse_frame(frame);
      if (!ev.is_null() && ev.value("name", "") == "hello") hello = ev["payload"];
    });
  }

  json call(const std::string& method, const json& params, const EventSink& on_event) override {
    const std::string rid = "r" + std::to_string(++next_id_);
    const json req = json{{"type", "request"}, {"id", rid}, {"method", method}, {"params", params}};
    json resp;
    engine_.handle_request(req, [&](const std::string& frame) {
      const json f = proto::parse_frame(frame);
      if (f.is_null()) return;
      if (f.value("type", "") == "event") {
        on_event(f);
      } else {
        resp = f;  // response 一定是最后到的那一帧（protocol.md §9 S2 的不变量）
      }
    });
    return resp;
  }

  int shutdown() override {
    const json bye = call("shutdown", json::object(), [](const json&) {});
    (void)bye;
    return 0;  // 没有子进程可回收；报 0 以保持与管道模式输出一致
  }

 private:
  core::CoreEngine engine_;
  int64_t next_id_ = 0;
};

// ============================ 命令行参数 ============================
struct CliArgs {
  bool embedded = false;
  bool demo = false;  // --demo：用 core 内置假模型（S2），否则用真模型 chat（S3a）
  std::string core_path;
  core::Options core_opts;
};

void usage() {
  std::cerr << "usage: agent-cli [--demo] [--embedded] [--base-url URL] [--model NAME]\n"
               "                  [--log-raw FILE] [--demo-delay-ms N] [--provider-timeout-ms N] [CORE_PATH]\n"
               "  --demo              用内置假模型（离线，S2 的 demo.chat）\n"
               "  --embedded          不 spawn 子进程，同进程直连 core（调试利器）\n"
               "  --base-url URL      真模型端点（默认 https://api.deepseek.com）\n"
               "  --model NAME        模型名（默认 deepseek-chat）\n"
               "  --no-stream         透传给 core：不用 SSE（回到 S3a 的整段返回）\n"
               "  --tools LIST        透传给 core：all 或逗号分隔（read_file,write_file,run_shell），默认不给\n"
               "  --workdir DIR       透传给 core：工具的相对路径锚点 / run_shell 的工作目录\n"
               "  --max-steps N       透传给 core：一个 turn 里最多几次模型调用（默认 8）\n"
               "  --log-raw FILE      透传给 core：抓双向报文\n"
               "  --demo-delay-ms N   透传给 core：假模型打字速度\n"
               "  API key 从环境变量 DEEPSEEK_API_KEY 读取（不会出现在命令行里）\n"
               "  CORE_PATH           agent-core 的路径（默认与本程序同目录）\n";
}

// 取开关的值：支持 --k=v 与 --k v 两种写法；命中返回 true
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
  const std::string self = (argv0 != nullptr) ? argv0 : "./agent-cli";
  const auto slash = self.find_last_of('/');
  return (slash == std::string::npos ? "." : self.substr(0, slash)) + "/agent-core";
}

}  // namespace

int main(int argc, char** argv) {
  CliArgs cli;
  bool core_path_set = false;
  for (int i = 1; i < argc; ++i) {
    std::string v;
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") { usage(); return 0; }
    else if (a == "--embedded") cli.embedded = true;
    else if (a == "--demo") cli.demo = true;
    else if (opt_value(argc, argv, i, "--base-url", v)) cli.core_opts.base_url = v;
    else if (opt_value(argc, argv, i, "--model", v)) cli.core_opts.model = v;
    else if (opt_value(argc, argv, i, "--provider-timeout-ms", v))
      cli.core_opts.provider_timeout_ms = std::max(1, std::atoi(v.c_str()));
    else if (a == "--no-stream") cli.core_opts.stream = false;
    else if (a == "--stream") cli.core_opts.stream = true;
    else if (opt_value(argc, argv, i, "--tools", v)) cli.core_opts.tools = core::parse_tool_list(v);
    else if (opt_value(argc, argv, i, "--workdir", v)) cli.core_opts.workdir = v;
    else if (opt_value(argc, argv, i, "--max-steps", v))
      cli.core_opts.max_steps = std::max(1, std::atoi(v.c_str()));
    else if (opt_value(argc, argv, i, "--log-raw", v)) cli.core_opts.log_raw_path = v;
    else if (opt_value(argc, argv, i, "--demo-delay-ms", v)) cli.core_opts.demo_delay_ms = std::max(0, std::atoi(v.c_str()));
    else if (!a.empty() && a[0] == '-') { std::cerr << "agent-cli: unknown option: " << a << "\n"; usage(); return 1; }
    else if (!core_path_set) { cli.core_path = a; core_path_set = true; }
    else { std::cerr << "agent-cli: too many arguments\n"; usage(); return 1; }
  }
  if (cli.core_path.empty()) cli.core_path = default_core_path(argc > 0 ? argv[0] : nullptr);

  std::unique_ptr<Session> session;
  try {
    session = cli.embedded ? std::unique_ptr<Session>(new EmbeddedSession(cli.core_opts))
                           : std::unique_ptr<Session>(new PipeSession(cli.core_path, cli.core_opts));
  } catch (const std::exception& e) {
    std::cerr << "agent-cli: " << e.what() << "\n";
    return 1;
  }

  // 握手第一步：core 的身份
  std::cout << "[core ready] protocol=" << session->hello.value("protocol_version", "?")
            << " core=" << session->hello.value("core_version", "?")
            << " pid=" << session->hello.value("pid", 0) << "\n" << std::flush;

  // 握手第二步：request hello（版本协商；成功响应即 welcome 语义）
  const json welcome = session->call("hello",
      json{{"protocol_version", proto::PROTOCOL_VERSION}, {"client_version", "0.0.1"}},
      [](const json&) {});
  if (welcome.is_null() || !welcome.value("ok", false)) {
    std::cerr << "agent-cli: handshake rejected: " << welcome.dump() << "\n";
    return 1;
  }
  std::cout << "[handshake] ok\n" << std::flush;

  // REPL：--demo 走 demo.chat（S2 的假流式），否则走 chat（S3a 的真模型；S3b 起默认流式）
  const std::string method = cli.demo ? "demo.chat" : "chat";
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    size_t deltas = 0;            // 本轮所有增量（demo 分支的结算行要用）
    size_t deltas_in_last_step = 0;  // **最后一个** model step 的增量数：决定结算时要不要重播全文
    size_t cur_step = 0;
    bool line_open = false;       // 输出是否停在一行中间
    const json resp = session->call(method, json{{"text", line}}, [&](const json& ev) {
      const std::string name = ev.value("name", "");
      const json payload = ev.contains("payload") ? ev["payload"] : json::object();

      if (name == "assistant.delta") {
        const size_t step = payload.value("step", 1);
        if (step != cur_step) {  // S4：新的 model step——另起一行，别把两轮说的话黏在一起
          if (line_open) std::cout << "\n";
          cur_step = step;
          deltas_in_last_step = 0;
          std::cout << "[assistant] " << std::flush;
          line_open = true;
        }
        std::cout << payload.value("text", "") << std::flush;  // 逐帧立刻输出 = 打字机
        ++deltas;
        ++deltas_in_last_step;
        return;
      }

      if (name == "tool.call") {  // S4：模型要用工具了（结构化参数，前端只负责显示）
        if (line_open) {
          std::cout << "\n";
          line_open = false;
        }
        std::cout << "[tool] " << payload.value("name", "?") << " "
                  << payload.value("arguments", json::object()).dump() << std::flush;
        line_open = true;
        return;
      }

      if (name == "tool.result") {  // S4：工具跑完了——ok/error 与耗时
        std::cout << " -> " << (payload.value("ok", false) ? "ok" : "error");
        if (!payload.value("ok", false)) std::cout << " (" << payload.value("error", "") << ")";
        std::cout << " " << payload.value("duration_ms", 0) << "ms\n" << std::flush;
        line_open = false;
        return;
      }
    });

    if (resp.is_null()) {
      std::cerr << "agent-cli: core died unexpectedly\n";
      return 1;
    }
    if (!resp.value("ok", false)) {
      std::cout << "\n[error] " << resp["error"].value("code", "?") << ": "
                << resp["error"].value("message", "?") << "\n" << std::flush;
      continue;
    }
    const json& res = resp["result"];
    if (cli.demo) {
      std::cout << "\n[turn " << res.value("turn_id", "?") << " done: " << deltas
                << " deltas, stop_reason=" << res.value("stop_reason", "?") << "]\n" << std::flush;
    } else {
      // S3b：流式时文字已经一个增量一个增量吐出来了，这里只换行；
      // 非流式（--no-stream / 对端无视 stream:true）才整段打印一次。
      // S4：判断依据是"**最后一个** step 有没有吐过字"——中间的 step 可能只说了一句就去调工具了。
      const std::string text = res.value("text", "");
      if (deltas_in_last_step == 0 && !text.empty()) {
        if (line_open) {
          std::cout << "\n";
          line_open = false;
        }
        std::cout << "[assistant] " << text << "\n";
      } else if (line_open) {
        std::cout << "\n";
        line_open = false;
      }
      std::cout << "[turn " << res.value("turn_id", "?") << " done: stop_reason="
                << res.value("stop_reason", "?");
      if (res.contains("usage") && res["usage"].is_object()) {
        const json& u = res["usage"];
        std::cout << ", tokens=" << u.value("prompt_tokens", 0) << "/" << u.value("completion_tokens", 0)
                  << "/" << u.value("total_tokens", 0);
      }
      // 只有真的多步/用过工具才多打两个数：单步对话的输出与 S3b 逐字节一致（老测试靠这个）
      const long long steps = res.value("steps", 1LL);
      const long long calls = res.value("tool_calls", 0LL);
      if (steps > 1 || calls > 0) std::cout << ", steps=" << steps << ", tools=" << calls;
      std::cout << "]\n" << std::flush;
    }
  }

  const int code = session->shutdown();
  std::cout << "[core exited cleanly, code=" << code << "]\n";
  return 0;
}
