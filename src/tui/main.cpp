// agent-tui —— S9 的 FTXUI 前端。
//
// 结构：
//   * 读取线程只做一件事：从 core 的 stdout 读帧，然后 screen.Post(...) 丢回 UI 线程；
//   * UI 线程持有 TuiModel（纯状态），FTXUI 只负责把状态画出来；
//   * 回车发 chat，Esc 取消（审批中则拒绝），Ctrl-C 退出；
//   * 审批弹窗直接用 y/n 回答——判定仍在 core，这里只负责"问"。
#include "common/protocol.hpp"
#include "tui/core_client.hpp"
#include "tui/textwrap.hpp"
#include "tui/tui_model.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ftxui;
using core::CoreProcess;
using core::TuiModel;
using core::wrap_text;
using proto::json;

namespace {

struct Args {
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
  std::string approval = "ask";
  int approval_timeout_ms = 120000;
  int max_steps = 8;
  int max_context_tokens = 32000;
  int keep_recent_turns = 3;
  int demo_delay_ms = 30;
  bool demo = false;
  bool stream = true;
};

void usage() {
  std::cerr
      << "usage: agent-tui [options]\n"
         "  --core PATH         agent-core 的路径（默认与本程序同目录）\n"
         "  --base-url URL      模型端点（默认 https://api.deepseek.com）\n"
         "  --model NAME        模型名（默认 deepseek-chat）\n"
         "  --demo              用 core 内置假模型（离线）\n"
         "  --tools LIST        all 或逗号分隔（read_file,write_file,run_shell），默认不给\n"
         "  --workdir DIR       工具的相对路径锚点\n"
         "  --system TEXT       系统提示词\n"
         "  --session ID        开/建会话\n"
         "  --resume[=ID]       恢复最近的会话，或指定已存在的 ID\n"
         "  --session-dir DIR   会话目录（默认 ~/.agent/sessions）\n"
         "  --approval MODE     ask（默认）/ never / allow\n"
         "  --approval-timeout-ms N  ask 等回答的上限（默认 120000）\n"
         "  --max-steps N       一个 turn 里最多几次模型调用（默认 8）\n"
         "  --max-context-tokens N  上下文预算（默认 32000）\n"
         "  --keep-recent-turns N   压缩时最近几轮原样保留（默认 3）\n"
         "  --no-stream         不用 SSE\n"
         "  API key 从环境变量 DEEPSEEK_API_KEY 读取\n";
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
  const std::string self = (argv0 != nullptr) ? argv0 : "./agent-tui";
  const auto slash = self.find_last_of('/');
  return (slash == std::string::npos ? "." : self.substr(0, slash)) + "/agent-core";
}

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

std::vector<std::string> core_args(const Args& a) {
  std::vector<std::string> args;
  args.push_back("--demo-delay-ms=" + std::to_string(a.demo_delay_ms));
  args.push_back(a.stream ? "--stream=true" : "--stream=false");
  args.push_back("--approval=" + a.approval);
  args.push_back("--approval-timeout-ms=" + std::to_string(a.approval_timeout_ms));
  args.push_back("--max-steps=" + std::to_string(a.max_steps));
  args.push_back("--max-context-tokens=" + std::to_string(a.max_context_tokens));
  args.push_back("--keep-recent-turns=" + std::to_string(a.keep_recent_turns));
  if (!a.base_url.empty()) args.push_back("--base-url=" + a.base_url);
  if (!a.model.empty()) args.push_back("--model=" + a.model);
  if (!a.tools.empty()) args.push_back("--tools=" + a.tools);
  if (!a.workdir.empty()) args.push_back("--workdir=" + a.workdir);
  if (!a.system_prompt.empty()) args.push_back("--system=" + a.system_prompt);
  if (!a.session_id.empty()) args.push_back("--session=" + a.session_id);
  if (!a.resume_id.empty()) args.push_back("--resume=" + a.resume_id);
  else if (a.resume_latest) args.push_back("--resume");
  if (!a.session_dir.empty()) args.push_back("--session-dir=" + a.session_dir);
  return args;
}

Element render_line(const core::TuiLine& line) {
  Element e = text(line.text);
  if (line.kind == "user") return e | color(Color::Cyan);
  if (line.kind == "assistant") return e;
  if (line.kind == "tool") return e | color(Color::Yellow);
  if (line.kind == "approval") return e | color(Color::Magenta);
  if (line.kind == "error") return e | color(Color::Red);
  return e | dim;
}

Element approval_modal(const json& ap, int width) {
  Elements lines;
  lines.push_back(text("审批请求") | bold);
  lines.push_back(separator());
  for (const std::string& part : wrap_text("工具: " + ap.value("name", "?"), width))
    lines.push_back(text(part));
  for (const std::string& part :
       wrap_text("参数: " + ap.value("arguments", json::object()).dump(), width))
    lines.push_back(text(part));
  for (const std::string& part : wrap_text("原因: " + ap.value("reason", ""), width))
    lines.push_back(text(part));
  lines.push_back(separator());
  lines.push_back(text("[y] 允许    [n] 拒绝    [Esc] 拒绝") | dim);
  return vbox(std::move(lines)) | border | bgcolor(Color::Black) |
         size(WIDTH, LESS_THAN, width + 2);
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string v;
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else if (arg == "--demo") a.demo = true;
    else if (arg == "--no-stream") a.stream = false;
    else if (arg == "--stream") a.stream = true;
    else if (arg == "--resume") a.resume_latest = true;
    else if (arg.rfind("--resume=", 0) == 0) a.resume_id = arg.substr(9);
    else if (opt_value(argc, argv, i, "--core", v)) a.core_path = v;
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
    else if (opt_value(argc, argv, i, "--max-steps", v))
      a.max_steps = std::max(1, std::atoi(v.c_str()));
    else if (opt_value(argc, argv, i, "--max-context-tokens", v))
      a.max_context_tokens = std::max(0, std::atoi(v.c_str()));
    else if (opt_value(argc, argv, i, "--keep-recent-turns", v))
      a.keep_recent_turns = std::max(1, std::atoi(v.c_str()));
    else if (opt_value(argc, argv, i, "--demo-delay-ms", v))
      a.demo_delay_ms = std::max(0, std::atoi(v.c_str()));
    else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "agent-tui: unknown option: " << arg << "\n";
      usage();
      return 1;
    } else {
      std::cerr << "agent-tui: unexpected argument: " << arg << "\n";
      return 1;
    }
  }
  if (a.core_path.empty()) a.core_path = default_core_path(argc > 0 ? argv[0] : nullptr);
  if (!a.session_id.empty() && (a.resume_latest || !a.resume_id.empty())) {
    std::cerr << "agent-tui: --session and --resume are mutually exclusive\n";
    return 1;
  }
  if (a.approval != "ask" && a.approval != "never" && a.approval != "allow" &&
      a.approval != "deny" && a.approval != "auto") {
    std::cerr << "agent-tui: --approval must be ask, never or allow\n";
    return 1;
  }

  try {
    CoreProcess core(a.core_path, core_args(a));
    auto screen = ScreenInteractive::Fullscreen();
    TuiModel model;
    model.set_session(core.hello().contains("session") ? core.hello()["session"] : json::object());

    std::string input;
    std::string chat_id;
    std::string cancel_id;
    std::atomic<bool> running{true};

    auto send_chat = [&] {
      if (model.busy() || model.approval_pending()) return;
      std::string text = trim(input);
      if (text.empty()) return;
      if (text == "/quit" || text == "/exit") {
        screen.Exit();
        return;
      }
      model.begin_turn(text);
      input.clear();
      chat_id = core.send(a.demo ? "demo.chat" : "chat", json{{"text", text}});
      screen.RequestAnimationFrame();
    };

    auto decide = [&](const std::string& decision) {
      if (!model.approval_pending()) return;
      const json ap = model.approval();
      if (ap.contains("approval_id"))
        core.send("approval.respond", json{{"approval_id", ap["approval_id"]},
                                          {"decision", decision}});
      model.on_approval_decided(decision);
      screen.RequestAnimationFrame();
    };

    auto cancel = [&] {
      if (!model.busy() || !cancel_id.empty()) return;
      cancel_id = core.send("turn.cancel", json::object());
      model.on_cancel_requested();
      screen.RequestAnimationFrame();
    };

    auto handle_frame = [&](const json& f) {
      const std::string type = f.value("type", "");
      if (type == "event") {
        model.on_event(f);
      } else if (type == "response") {
        const std::string id = f.value("id", "");
        if (id == chat_id) {
          model.on_response(f);
          chat_id.clear();
        } else if (id == cancel_id) {
          cancel_id.clear();
        }
      }
      screen.RequestAnimationFrame();
    };

    InputOption input_option;
    input_option.content = &input;
    input_option.placeholder = "输入消息，Enter 发送，Esc 取消，Ctrl-C 退出";
    input_option.multiline = false;
    input_option.on_enter = [&] { send_chat(); };
    Component input_comp = Input(input_option);

    auto renderer = Renderer(input_comp, [&] {
        const auto dims = Terminal::Size();
        const int content_width = std::max(10, dims.dimx - 2);

        // 先按显示宽度展开，再取最后 N 行；否则长中文/长 JSON 会横向溢出。
        std::vector<core::TuiLine> visible;
        for (const auto& line : model.lines()) {
          for (const std::string& part : wrap_text(line.text, content_width))
            visible.push_back(core::TuiLine{line.kind, part});
        }
        if (visible.empty()) {
          for (const std::string& part : wrap_text("输入一句话开始；/quit 退出", content_width))
            visible.push_back(core::TuiLine{"system", part});
        }

        size_t max_lines = dims.dimy > 10 ? static_cast<size_t>(dims.dimy - 8) : 8;
        size_t start = visible.size() > max_lines ? visible.size() - max_lines : 0;
        Elements body;
        for (size_t i = start; i < visible.size(); ++i) body.push_back(render_line(visible[i]));

        std::string session = model.session_id().empty() ? "no session" : model.session_id();
        std::string status = model.status() + "  |  session " + session + "  |  turns " +
                             std::to_string(model.turns());
        Elements status_body;
        for (const std::string& part : wrap_text(status, content_width))
          status_body.push_back(text(part) | dim);

        Element doc = vbox({
            text("yxAgent TUI  (S9)") | bold,
            separator(),
            vbox(std::move(body)) | flex,
            separator(),
            hbox({text("> ") | bold, input_comp->Render() | flex}),
            separator(),
            vbox(std::move(status_body)),
        });
        if (model.approval_pending()) {
          const int modal_width = std::max(24, std::min(76, dims.dimx - 4));
          doc = dbox({doc, approval_modal(model.approval(), modal_width - 2) | center});
        }
        return doc;
      });

    auto root = CatchEvent(renderer, [&](Event e) {
      if (e == Event::Special(std::string(1, '\x03')) || e.input() == std::string(1, '\x03')) {
        screen.Exit();
        return true;
      }
      if (e == Event::Escape) {
        if (model.approval_pending())
          decide("deny");
        else if (model.busy())
          cancel();
        return true;
      }
      if (model.approval_pending()) {
        if (e == Event::Character('y') || e == Event::Character('Y')) {
          decide("allow");
          return true;
        }
        if (e == Event::Character('n') || e == Event::Character('N')) {
          decide("deny");
          return true;
        }
        if (e == Event::Return) return true;
      }
      return false;
    });

    std::thread reader([&] {
      json f;
      while (running && core.read_frame(f))
        screen.Post([&, f] { handle_frame(f); });
      if (running) {
        screen.Post([&] {
          model.on_core_died("core closed stdout");
          screen.Exit();
        });
      }
    });

    screen.Loop(root);

    running = false;
    try {
      core.send("shutdown", json::object());
    } catch (...) {
    }
    core.stop();  // 关闭 stdin；若 core 还在忙，500ms 后 SIGTERM
    if (reader.joinable()) reader.join();

    std::cout << "\n[agent-tui exited]\n" << std::flush;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "agent-tui: " << e.what() << "\n";
    return 1;
  }
}