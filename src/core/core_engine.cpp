#include "core/core_engine.hpp"

#include "core/openai_compat.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace core {
namespace {

// —— S2 的假模型：确定性、可测、按"码点"吐字 ——
// 它存在的唯一理由：让 core → cli 的事件流先跑通。S3 会把它换成真 Provider，
// 而帧的形状不变——这就是先做假模型的价值（一次只调一个未知数）。

// 把 UTF-8 字符串切成码点（不是字节！）。
// 为什么必须按码点：一个汉字是 3 字节，按字节切会让"半个汉字"进入一个 delta，
// 前端就会画出乱码。S3 那条"chunk 切在 UTF-8 码点中间"的 fixture 就是这件事的真实版。
std::vector<std::string> split_utf8_codepoints(const std::string& s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 1;
    if ((c & 0x80) == 0x00)      len = 1;  // 0xxxxxxx：ASCII
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;  // 注意是F8，通过连续的1和后面的一个0来进行判断的
    else                         len = 1;  // 非法起始字节：原样单字节吐，绝不卡死
    if (i + len > s.size())      len = 1;  // 被截断的序列：同样单字节吐
    out.push_back(s.substr(i, len));
    i += len;
  }
  return out;
}

// 报错里只想看开头一点：不要把整段模型输出糊进一条错误消息
std::string truncate(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  return s.substr(0, n) + "...";
}

// 启用工具但没给 --system 时的默认提示词。
// 为什么需要它：模型是无状态的，"你能用工具"这件事光靠请求里的 tools 数组还不够——
// tools 只说明"有哪些函数"，不说"什么时候该用、结果可不可信"。这两句话必须在提示词里讲清楚。
std::string default_agent_prompt(const Options& opts) {
  std::string p =
      "你是一个能使用工具的 agent。\n"
      "- 需要读文件、写文件或执行命令时，直接调用工具；不要凭空猜测文件内容或命令输出。\n"
      "- 工具返回的结果是唯一可信的事实；失败时（ERROR: ...）先读错误，再决定换参数还是换做法。\n"
      "- 拿到足够信息就给结论，不要反复调用同一个工具。\n";
  if (!opts.workdir.empty()) p += "- 工作目录：" + opts.workdir + "（相对路径都相对于它）。\n";
  p += "- 用用户的语言回答，简洁。\n";
  return p;
}

std::string demo_reply(const std::string& text, int64_t turn) {
  return "收到第 " + std::to_string(turn) + " 句：「" + text +
         "」—— 我是 S2 的假模型，只会把你说的话原样还给你；真模型在 S3 接入。";
}

}  // namespace

CoreEngine::CoreEngine(Options opts) : opts_(std::move(opts)) {
  if (!opts_.log_raw_path.empty()) {
    log_.open(opts_.log_raw_path, std::ios::out | std::ios::trunc);
    if (!log_) std::cerr << "[core] warning: cannot open log-raw file: " << opts_.log_raw_path << "\n";
  }

  // S3a：有 base_url 或 api_key 就建 provider；两者都没有 = 纯 demo 模式
  const bool want_provider = !opts_.base_url.empty() || !opts_.api_key.empty();
  if (want_provider) {
    provider_ = std::make_unique<OpenAICompatProvider>(OpenAIConfig{
        opts_.base_url.empty() ? "https://api.deepseek.com" : opts_.base_url,
        opts_.api_key,
        opts_.model.empty() ? "deepseek-chat" : opts_.model,
        opts_.provider_timeout_ms});
  }

  // S4：把 --tools 点名的工具装进注册表。名字不认识不致命（引擎少一件手脚），但必须吵：
  // 静默丢一个工具，模型会一直抱怨"我没有这个能力"，而人以为是模型笨。
  for (const std::string& name : opts_.tools) {
    auto tool = make_builtin_tool(name, opts_.workdir);
    if (!tool) {
      std::cerr << "[core] warning: unknown tool ignored: " << name << " (known: read_file, write_file, run_shell)" << "\n";
      continue;
    }
    tools_.add(std::move(tool));
  }

  // 系统提示词进历史第 0 条。注意：只在"有工具"或"显式给了 --system"时才加——
  // S1–S3b 的假 server 会数消息条数（"fake(1 msgs)"），凭空多一条 system 会让它们全红。
  // 回滚用 history_.resize(history_mark)，而 mark 至少是 1，所以这条永远不会被滚掉。
  std::string system_prompt = opts_.system_prompt;
  if (system_prompt.empty() && !tools_.empty()) system_prompt = default_agent_prompt(opts_);
  if (!system_prompt.empty()) history_.push_back(Message{"system", system_prompt});
}

CoreEngine::~CoreEngine() = default;

json CoreEngine::hello_payload() const {
  return json{{"protocol_version", proto::PROTOCOL_VERSION},
              {"core_version", proto::CORE_VERSION},
              {"pid", static_cast<int64_t>(::getpid())}};
}

void CoreEngine::greet(const FrameSink& sink) {
  const std::string frame = proto::frame_event(++seq_, "hello", hello_payload());
  log('<', frame);
  sink(frame);
}

void CoreEngine::log(char dir, const std::string& frame) {
  if (!log_.is_open()) return;
  std::string f = frame;
  while (!f.empty() && (f.back() == '\n' || f.back() == '\r')) f.pop_back();  // 一帧 = 一行
  log_ << (dir == '>' ? "--> " : "<-- ") << f << '\n';
  log_.flush();  // 实时落盘：崩溃时也能看到最后一帧
}

void CoreEngine::handle_request(const json& req, const FrameSink& sink) {
  log('>', req.dump());
  const auto emit = [&](const std::string& frame) {
    log('<', frame);
    sink(frame);
  };
  const auto send_err = [&](const json& id, const std::string& code, const std::string& msg) {
    emit(proto::frame_response_err(id, code, msg));
  };

  const json& id = req["id"];
  const std::string method = req["method"].get<std::string>();
  const json params = (req.contains("params") && req["params"].is_object()) ? req["params"] : json::object();

  if (method == "hello") {
    const std::string pv = (params.contains("protocol_version") && params["protocol_version"].is_string())
                             ? params["protocol_version"].get<std::string>() : "";
    if (proto::major_of(pv) != proto::major_of(proto::PROTOCOL_VERSION)) {
      send_err(id, "PROTOCOL_VERSION_MISMATCH",
               "core speaks " + std::string(proto::PROTOCOL_VERSION) + ", client sent " + pv);
      exit_code_ = 2;  // protocol.md §4.2：major 不兼容 -> core 退出码 2
      shutdown_requested_ = true;
      return;
    }
    emit(proto::frame_response_ok(id, json{{"protocol_version", proto::PROTOCOL_VERSION},
                                           {"core_version", proto::CORE_VERSION}}));
    return;
  }

  if (method == "ping") {
    json result = json::object();
    if (params.contains("client_ts")) result["client_ts"] = params["client_ts"];
    result["core_ts"] = static_cast<int64_t>(std::time(nullptr));
    emit(proto::frame_response_ok(id, result));
    return;
  }

  if (method == "echo") {  // S1 的调试消息，S2 起被 demo.chat 取代但保留（老测试还在用）
    emit(proto::frame_response_ok(id, params));
    return;
  }

  if (method == "demo.chat") {
    const std::string text = (params.contains("text") && params["text"].is_string())
                               ? params["text"].get<std::string>() : "";
    const int64_t turn = ++turn_;
    const std::string turn_id = "t" + std::to_string(turn);
    const std::string reply = demo_reply(text, turn);

    // 一个请求 → 一串事件 → 最后才响应（protocol.md §9 S2）
    for (const std::string& cp : split_utf8_codepoints(reply)) {
      emit(proto::frame_event(++seq_, "assistant.delta", json{{"turn_id", turn_id}, {"text", cp}}));
      if (opts_.demo_delay_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(opts_.demo_delay_ms));
    }
    emit(proto::frame_response_ok(id, json{{"turn_id", turn_id}, {"text", reply}, {"stop_reason", "stop"}}));
    return;
  }

  if (method == "chat") {
    if (!provider_) {
      send_err(id, "NO_PROVIDER",
               "core has no model provider: start with --base-url https://api.deepseek.com --model deepseek-chat "
               "and an api key in DEEPSEEK_API_KEY (or use --demo for the built-in fake model)");
      return;
    }
    const std::string text = (params.contains("text") && params["text"].is_string())
                               ? params["text"].get<std::string>() : "";
    const int64_t turn = ++turn_;
    const std::string turn_id = "t" + std::to_string(turn);

    // 失败要整轮回滚：这一轮可能往历史里塞了 user / assistant(tool_calls) / 若干 tool 消息。
    // 只 pop 一条是不够的（S3a 的契约：失败的一轮不得污染下一次请求）。
    const size_t history_mark = history_.size();
    history_.push_back(Message{"user", text});

    // S3b：流式时每个增量立刻变成一个 assistant.delta。
    // 顺序：delta 必须在本轮 response **之前**发完，response 永远是最后一帧（protocol.md §9 不变量 1）。
    const bool use_stream = opts_.stream && provider_->supports_stream();
    const std::vector<ToolSpec> specs = tools_.specs();
    size_t tool_calls_total = 0;

    try {
      for (int step = 1;; ++step) {
        if (step > opts_.max_steps) {
          // 循环硬上限：模型一直要调工具就是不肯收尾，这时候必须有人喊停。
          history_.resize(history_mark);
          send_err(id, "MAX_STEPS", "agent loop stopped after " + std::to_string(opts_.max_steps) +
                                        " model calls without a final answer (raise --max-steps if this is expected)");
          return;
        }

        ChatRequest req;
        req.messages = history_;
        req.tools = specs;

        // delta 带 step：一个 turn 可能有多个 step，前端要知道"这句话是哪一轮说的"
        const auto on_delta = [&](const std::string& piece) {
          emit(proto::frame_event(++seq_, "assistant.delta",
                                  json{{"turn_id", turn_id}, {"step", step}, {"text", piece}}));
        };
        const ChatResult r = use_stream ? provider_->chat_stream(req, on_delta) : provider_->chat(req);

        if (r.tool_calls.empty()) {  // 模型给出最终答复：本 turn 结束
          history_.push_back(Message{"assistant", r.text});
          emit(proto::frame_response_ok(id, json{{"turn_id", turn_id},
                                                 {"text", r.text},
                                                 {"stop_reason", r.finish_reason},
                                                 {"usage", r.usage},
                                                 {"steps", step},
                                                 {"tool_calls", static_cast<int64_t>(tool_calls_total)}}));
          return;
        }

        // 模型要调工具：先把这条 assistant 消息（带 tool_calls）记进历史，
        // 否则下一轮模型不知道自己刚才要求过什么。
        Message assistant{"assistant", r.text};
        assistant.tool_calls = r.tool_calls;
        history_.push_back(assistant);

        for (const ToolCall& call : r.tool_calls) {
          ++tool_calls_total;

          // 模型的 arguments 是 JSON 文本：坏 JSON 不是崩溃的理由，而是要告诉模型"你写错了"
          json args = json::object();
          std::string arg_error;
          if (call.arguments.empty()) {
            args = json::object();
          } else {
            args = json::parse(call.arguments, nullptr, false);
            if (args.is_discarded() || !args.is_object()) {
              arg_error = "arguments are not a valid JSON object";
              args = json::object();
            }
          }

          json call_payload = {{"turn_id", turn_id},
                               {"step", step},
                               {"call_id", call.id},
                               {"name", call.name},
                               {"arguments", args}};
          if (!arg_error.empty()) call_payload["arguments_raw"] = call.arguments;
          emit(proto::frame_event(++seq_, "tool.call", call_payload));

          const auto started = std::chrono::steady_clock::now();
          const ToolResult res = arg_error.empty()
                                     ? tools_.execute(call.name, args)
                                     : ToolResult::failure(arg_error + ": " + truncate(call.arguments, 200));
          const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started).count();

          emit(proto::frame_event(++seq_, "tool.result",
                                  json{{"turn_id", turn_id},
                                       {"step", step},
                                       {"call_id", call.id},
                                       {"name", call.name},
                                       {"ok", res.ok},
                                       {"output", res.output},
                                       {"error", res.error},
                                       {"duration_ms", static_cast<int64_t>(elapsed)}}));

          // 工具结果回灌历史。**失败也要回灌**：ERROR 是模型自我纠正的唯一线索。
          std::string content;
          if (res.ok) {
            content = res.output;
          } else {
            content = "ERROR: " + res.error;
            if (!res.output.empty()) content += "\n" + res.output;
          }
          Message tool_msg{"tool", content};
          tool_msg.tool_call_id = call.id;
          tool_msg.name = call.name;
          history_.push_back(tool_msg);
        }
      }
    } catch (const std::exception& e) {
      history_.resize(history_mark);  // 失败的一轮不进历史：否则下一次请求会带着半截对话
      send_err(id, "PROVIDER_ERROR", e.what());
    }
    return;
  }

  if (method == "shutdown") {
    emit(proto::frame_response_ok(id, json{{"will_exit", true}}));
    shutdown_requested_ = true;
    return;
  }

  send_err(id, "UNKNOWN_METHOD", "no such method: " + method);
}

int CoreEngine::run(std::istream& in, std::ostream& out) {
  const auto raw_write = [&](const std::string& frame) { out << frame << std::flush; };
  const auto emit = [&](const std::string& frame) { log('<', frame); raw_write(frame); };

  // 第一帧永远是 event hello（protocol.md §4.1）
  greet(raw_write);

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;  // 忍耐空行，不打断会话

    const json v = proto::parse_frame(line);
    if (v.is_null()) {
      log('>', line);
      emit(proto::frame_response_err(json(), "PARSE_ERROR", "frame is not a valid JSON object"));
      continue;
    }
    const std::string type = (v.contains("type") && v["type"].is_string()) ? v["type"].get<std::string>() : "";
    if (type != "request") {  // §6 未知即忽略：不响应
      log('>', line);
      continue;
    }
    if (!proto::is_request(v)) {
      log('>', line);
      emit(proto::frame_response_err(proto::extract_id(v), "INVALID_ENVELOPE",
                                     "expected a request envelope (type=request with string id and method)"));
      continue;
    }
    handle_request(v, raw_write);  // 它内部的 emit 负责记日志，别在这里重复记
    if (shutdown_requested_) break;
  }

  out << std::flush;  // 退出前必须 flush（protocol.md §4.3 铁律）
  return exit_code_;
}

}  // namespace core
