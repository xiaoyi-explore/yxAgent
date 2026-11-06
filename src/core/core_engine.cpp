#include "core/core_engine.hpp"

#include "common/utf8.hpp"
#include "core/openai_compat.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <memory>
#include <poll.h>
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
  return s.substr(0, proto::utf8_prefix_bytes(s, n)) + "...";
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

CoreEngine::CoreEngine(Options opts)
    : opts_(std::move(opts)),
      // S6：阈值 0.5 是 Gemini CLI 的实测值；等到快满了再压，你会先被"一次多轮工具调用"顶爆。
      ctx_(ContextConfig{opts_.max_context_tokens, 0.5, opts_.keep_recent_turns, 2048}),
      policy_(opts_.workdir, opts_.approval_mode) {
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
  configured_system_ = system_prompt;
  if (!system_prompt.empty()) history_.push_back(Message{"system", system_prompt});

  // S7：会话在构造时 attach，必须早于 greet()——这样 resume 后的 hello 也接着会话 seq。
  attach_session(history_);
}

CoreEngine::~CoreEngine() = default;

void CoreEngine::attach_session(const std::vector<Message>& initial_messages) {
  if (opts_.session_id.empty() && opts_.resume_id.empty() && !opts_.resume_latest) return;

  const std::string dir = opts_.session_dir.empty() ? default_session_dir() : opts_.session_dir;
  const SessionHeaderConfig cfg{opts_.model, opts_.workdir, opts_.tools};
  auto store = std::make_unique<SessionStore>(dir);

  SessionOpenResult res;
  if (!opts_.resume_id.empty())
    res = store->open(opts_.resume_id, false, initial_messages, cfg);
  else if (opts_.resume_latest)
    res = store->open_latest(initial_messages, cfg);
  else
    res = store->open(opts_.session_id, true, initial_messages, cfg);

  if (!res.created) {
    history_ = std::move(res.messages);
    if (!configured_system_.empty() &&
        (history_.empty() || history_.front().role != "system")) {
      std::cerr << "[session] warning: resumed session has no system message; the current "
                   "--system/--tools prompt is not applied (start a new session to change it)\n";
    }
  }

  // seq 只增不减：同一个连接里 session.load 到较旧的会话也不许回头。
  seq_ = std::max(seq_, res.last_seq);
  turn_ = std::max(turn_, res.turns);
  session_ = std::move(store);
  std::cerr << "[session] " << (res.resumed ? "resumed" : "new") << " " << res.id
            << " (" << turn_ << " turns, seq " << seq_ << ")\n";
}

void CoreEngine::session_turn_begin(int64_t turn) {
  if (!session_) return;
  try {
    session_->append_turn_begin(turn);
  } catch (const std::exception& e) {
    if (!session_warned_) {
      session_warned_ = true;
      std::cerr << "[session] warning: cannot record turn begin: " << e.what() << "\n";
    }
  }
}

void CoreEngine::session_commit(int64_t turn, const std::string& stop_reason, const json& usage,
                                int64_t steps, int64_t tool_calls, size_t history_mark,
                                bool compacted) {
  if (!session_) return;
  try {
    if (compacted) {
      session_->append_commit(turn, stop_reason, usage, steps, tool_calls, {}, &history_);
    } else {
      if (history_mark > history_.size()) history_mark = history_.size();
      std::vector<Message> added(history_.begin() + static_cast<std::ptrdiff_t>(history_mark),
                                 history_.end());
      session_->append_commit(turn, stop_reason, usage, steps, tool_calls, added, nullptr);
    }
  } catch (const std::exception& e) {
    if (!session_warned_) {
      session_warned_ = true;
      std::cerr << "[session] warning: cannot commit turn " << turn << ": " << e.what() << "\n";
    }
  }
}

void CoreEngine::session_abort(int64_t turn, const std::string& code, bool compacted) {
  if (!session_) return;
  try {
    if (compacted)
      session_->append_abort(turn, code, &history_);
    else
      session_->append_abort(turn, code, nullptr);
  } catch (const std::exception& e) {
    if (!session_warned_) {
      session_warned_ = true;
      std::cerr << "[session] warning: cannot abort turn " << turn << ": " << e.what() << "\n";
    }
  }
}

void CoreEngine::resolve_approval(const json& req) {
  const json& id = req["id"];
  const json params = (req.contains("params") && req["params"].is_object()) ? req["params"] : json::object();
  const std::string approval_id = (params.contains("approval_id") && params["approval_id"].is_string())
                                      ? params["approval_id"].get<std::string>() : std::string();

  if (pending_approval_id_.empty() || approval_done_ || approval_id != pending_approval_id_) {
    send(proto::frame_response_err(id, "NO_PENDING_APPROVAL",
                                   "no approval is pending (or it was already answered); "
                                   "wait for approval.requested first"));
    return;
  }

  const std::string decision = (params.contains("decision") && params["decision"].is_string())
                                   ? params["decision"].get<std::string>() : std::string();
  if (decision != "allow" && decision != "deny") {
    send(proto::frame_response_err(id, "INVALID_PARAMS",
                                   "approval.respond decision must be 'allow' or 'deny'"));
    return;
  }

  approval_done_ = true;
  approval_allowed_ = (decision == "allow");
  approval_answer_reason_ = (params.contains("reason") && params["reason"].is_string())
                                ? params["reason"].get<std::string>() : std::string();
  send(proto::frame_response_ok(id, json{{"approval_id", approval_id}, {"decision", decision}}));
}

bool CoreEngine::request_approval(const json& payload, std::string& reason) {
  const std::string approval_id = (payload.contains("approval_id") && payload["approval_id"].is_string())
                                      ? payload["approval_id"].get<std::string>() : std::string();
  if (approval_id.empty()) {
    reason = "approval payload has no approval_id";
    return false;
  }
  if (cancel_requested_) {
    reason = "cancelled before approval";
    return false;
  }

  pending_approval_id_ = approval_id;
  approval_done_ = false;
  approval_allowed_ = false;
  approval_answer_reason_.clear();
  send(proto::frame_event(++seq_, "approval.requested", payload));

  // --embedded 没有"等回答"的窗口：同步回调里再读 stdin 会和 cli 抢输入。
  // 明确拒绝，而不是假装批准；理由会作为工具错误回灌给模型。
  if (!stdio_mode_) {
    pending_approval_id_.clear();
    reason = "--embedded does not support interactive approval";
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(std::max(0, opts_.approval_timeout_ms));
  while (!approval_done_) {
    pump_input();
    if (approval_done_) break;
    if (cancel_requested_) {
      reason = "cancelled during approval";
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      reason = "approval timed out after " + std::to_string(opts_.approval_timeout_ms) + " ms";
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  const bool allowed = approval_done_ && approval_allowed_;
  if (!allowed && reason.empty()) {
    if (approval_done_)
      reason = approval_answer_reason_.empty() ? "user denied" : approval_answer_reason_;
    else
      reason = "no approval decision";
  }
  pending_approval_id_.clear();
  approval_done_ = false;
  return allowed;
}

json CoreEngine::hello_payload() const {
  json p = {{"protocol_version", proto::PROTOCOL_VERSION},
            {"core_version", proto::CORE_VERSION},
            {"pid", static_cast<int64_t>(::getpid())}};
  if (session_) p["session"] = session_->info(false);
  return p;
}

void CoreEngine::greet(const FrameSink& sink) {
  const std::string frame = proto::frame_event(++seq_, "hello", hello_payload());
  log('<', frame);
  sink(frame);
  if (session_) {
    try {
      session_->append_session_open(false);
    } catch (const std::exception& e) {
      if (!session_warned_) {
        session_warned_ = true;
        std::cerr << "[session] warning: cannot record session.open: " << e.what() << "\n";
      }
    }
  }
}

void CoreEngine::log(char dir, const std::string& frame) {
  std::string f = frame;
  while (!f.empty() && (f.back() == '\n' || f.back() == '\r')) f.pop_back();  // 一帧 = 一行

  if (log_.is_open()) {
    log_ << (dir == '>' ? "--> " : "<-- ") << f << '\n';
    log_.flush();  // 实时落盘：崩溃时也能看到最后一帧
  }

  // S7：会话日志顺手记线上帧（审计 + seq 水位）。恢复不读这些帧。
  if (session_ && !f.empty()) {
    const json parsed = proto::parse_frame(f);
    if (!parsed.is_null()) {
      try {
        session_->append_frame(parsed);
      } catch (const std::exception& e) {
        if (!session_warned_) {
          session_warned_ = true;
          std::cerr << "[session] warning: cannot append to session log: " << e.what() << "\n";
        }
      }
    }
  }
}

void CoreEngine::send(const std::string& frame) {
  log('<', frame);
  if (sink_) sink_(frame);
}

// ---------------------------------------------------------------- S5：读 stdin 的两张面孔

// 阻塞到凑够一整行。返回 false = EOF（cli 关掉了管道）。
bool CoreEngine::read_next_line(std::string& line) {
  for (;;) {
    const size_t pos = in_buf_.find('\n');
    if (pos != std::string::npos) {
      line = in_buf_.substr(0, pos);
      in_buf_.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return true;
    }
    struct pollfd p{STDIN_FILENO, POLLIN, 0};
    const int rc = ::poll(&p, 1, -1);
    if (rc < 0) {
      if (errno == EINTR) continue;  // 被信号打断：重来
      return false;
    }
    char chunk[4096];
    const ssize_t got = ::read(STDIN_FILENO, chunk, sizeof chunk);
    if (got <= 0) return false;  // EOF（或真的读错了：都当结束）
    in_buf_.append(chunk, static_cast<size_t>(got));
  }
}

// 非阻塞：有多少读多少。**这是 S5 取消能做到的全部秘密**——
// 一轮正在跑（甚至正卡在 HTTP 读上）的时候，增量回调里顺手调它一次，
// 就能把 cli 刚写进来的 turn.cancel 捞出来。
void CoreEngine::pump_input() {
  if (!stdio_mode_) return;  // embedded：stdin 是 cli 的，绝不去抢
  for (;;) {
    struct pollfd p{STDIN_FILENO, POLLIN, 0};
    if (::poll(&p, 1, 0) <= 0) break;  // 0 = 现在没有，立刻回去干活
    char chunk[4096];
    const ssize_t got = ::read(STDIN_FILENO, chunk, sizeof chunk);
    if (got <= 0) break;
    in_buf_.append(chunk, static_cast<size_t>(got));
  }
  size_t pos;
  while ((pos = in_buf_.find('\n')) != std::string::npos) {
    std::string line = in_buf_.substr(0, pos);
    in_buf_.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    dispatch_line(line);
  }
}

// 一行 -> 立刻处理 / 排队 / 取消
void CoreEngine::dispatch_line(const std::string& line) {
  const json v = proto::parse_frame(line);
  if (v.is_null()) {
    log('>', line);
    send(proto::frame_response_err(json(), "PARSE_ERROR", "frame is not a valid JSON object"));
    return;
  }
  const std::string type = (v.contains("type") && v["type"].is_string()) ? v["type"].get<std::string>() : "";
  if (type != "request") {  // §6 未知即忽略：不响应
    log('>', line);
    return;
  }
  if (!proto::is_request(v)) {
    log('>', line);
    send(proto::frame_response_err(proto::extract_id(v), "INVALID_ENVELOPE",
                                   "expected a request envelope (type=request with string id and method)"));
    return;
  }

  const std::string method = v["method"].get<std::string>();
  if (method == "approval.respond") {
    // 审批回答必须立刻处理：它和 turn.cancel 一样，是"本轮正在等的东西"。
    log('>', v.dump());
    resolve_approval(v);
    return;
  }
  if (method == "turn.cancel") {
    // 取消是**唯一**能在本轮进行中被处理的请求：它的全部意义就是"打断正在跑的东西"。
    // 其他请求一律排队（协议保证每个请求都有响应，只是晚一点）。
    log('>', v.dump());
    const bool was_active = turn_active_ != 0;
    cancel_requested_ = true;  // 空闲时置位也无害：下一轮开头会清掉
    json result = {{"cancelled", was_active}};
    if (was_active) result["turn"] = turn_active_;
    send(proto::frame_response_ok(v["id"], result));
    return;
  }
  deferred_.push_back(v);
}

// ---------------------------------------------------------------- 协议分发

void CoreEngine::handle_request(const json& req, const FrameSink& sink) {
  sink_ = sink;  // pump_input 里的 ack 也要走同一条出口
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
    json result = {{"protocol_version", proto::PROTOCOL_VERSION},
                   {"core_version", proto::CORE_VERSION}};
    if (session_) result["session"] = session_->info(false);
    emit(proto::frame_response_ok(id, result));
    return;
  }

  if (method == "session.load") {
    const bool resume = params.contains("resume") && params["resume"].is_boolean()
                            ? params["resume"].get<bool>() : false;
    const std::string sid = (params.contains("id") && params["id"].is_string())
                                ? params["id"].get<std::string>() : std::string();
    const bool create = params.contains("create") && params["create"].is_boolean()
                            ? params["create"].get<bool>() : false;
    if (resume && !sid.empty()) {
      send_err(id, "INVALID_PARAMS", "session.load takes either 'id' or 'resume', not both");
      return;
    }
    if (!resume && sid.empty()) {
      send_err(id, "INVALID_PARAMS", "session.load needs an 'id' (or resume:true)");
      return;
    }
    if (session_ && resume) {
      // 已经挂着一个会话：resume 视为"保持当前"（真正的切换用 id）。
      // 不能在这里重新 open_latest：同一进程对同一个文件再 flock 会得到 SESSION_BUSY。
      json info = session_->info(true);
      info["created"] = false;
      emit(proto::frame_response_ok(id, info));
      return;
    }
    if (session_ && !resume && sid == session_->id()) {
      json info = session_->info(true);
      info["created"] = false;
      emit(proto::frame_response_ok(id, info));
      return;
    }

    const std::string dir = opts_.session_dir.empty() ? default_session_dir() : opts_.session_dir;
    const SessionHeaderConfig cfg{opts_.model, opts_.workdir, opts_.tools};
    try {
      auto store = std::make_unique<SessionStore>(dir);
      // 新建会话不能把当前会话的 history_ 克隆过去；只用本次 flags 的 system 起步。
      std::vector<Message> initial;
      if (!configured_system_.empty()) initial.push_back(Message{"system", configured_system_});
      SessionOpenResult res = resume ? store->open_latest(initial, cfg)
                                     : store->open(sid, create, initial, cfg);
      history_ = std::move(res.messages);
      if (!configured_system_.empty() &&
          (history_.empty() || history_.front().role != "system")) {
        std::cerr << "[session] warning: loaded session has no system message; the current "
                     "--system/--tools prompt is not applied\n";
      }
      seq_ = std::max(seq_, res.last_seq);
      turn_ = std::max(turn_, res.turns);
      session_ = std::move(store);
      json info = session_->info(true);
      info["created"] = res.created;
      emit(proto::frame_response_ok(id, info));
    } catch (const SessionError& e) {
      send_err(id, e.code, e.what());
    } catch (const std::exception& e) {
      send_err(id, "SESSION_IO_ERROR", e.what());
    }
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
    session_turn_begin(turn);

    // 一个请求 → 一串事件 → 最后才响应（protocol.md §9 S2）
    // S5 刻意**不**给假模型合批：test_s2 用"一个 delta 恰好一个码点"钉着它，
    // 那是 S2 的验收基线，不该为了新阶段的功能去动老契约。
    for (const std::string& cp : split_utf8_codepoints(reply)) {
      emit(proto::frame_event(++seq_, "assistant.delta", json{{"turn_id", turn_id}, {"text", cp}}));
      if (opts_.demo_delay_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(opts_.demo_delay_ms));
    }
    if (session_)
      session_->append_commit(turn, "stop", json::object(), 1, 0, {}, nullptr);
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
    // 注意：**取消不走这条**——取消是用户喊停，已经发生的部分要留下（见下面的注释）。
    // S6 提醒：压缩会让 history_ **变短**，所以这个水位线不是 const——压完要跟着往下挪。
    size_t history_mark = history_.size();
    bool compacted = false;
    history_.push_back(Message{"user", text});
    session_turn_begin(turn);

    // S5：把"这一轮可以被取消"这件事告诉 turn.cancel 的处理者；~TurnFlag 负责复位。
    struct TurnFlag {
      int64_t& slot;
      explicit TurnFlag(int64_t& s) : slot(s) {}
      ~TurnFlag() { slot = 0; }
    } turn_flag{turn_active_};
    turn_active_ = turn;
    cancel_requested_ = false;  // 清掉上一轮遗留

    // S3b：流式时增量变成 assistant.delta。
    // 顺序：delta 必须在本轮 response **之前**发完，response 永远是最后一帧（protocol.md §9 不变量 1）。
    const bool use_stream = opts_.stream && provider_->supports_stream();
    const std::vector<ToolSpec> specs = tools_.specs();
    size_t tool_calls_total = 0;

    // S5 合批：待发的一小批增量。跨 step 复用，但**每个 model call 结束前必须清空**
    // （否则 result.text 与拼接对不上 = 不变量 2 破了）。
    std::string pending;
    auto last_flush = std::chrono::steady_clock::now();

    try {
      for (int step = 1;; ++step) {
        if (step > opts_.max_steps) {
          // 循环硬上限：模型一直要调工具就是不肯收尾，这时候必须有人喊停。
          history_.resize(history_mark);
          session_abort(turn, "MAX_STEPS", compacted);
          send_err(id, "MAX_STEPS", "agent loop stopped after " + std::to_string(opts_.max_steps) +
                                        " model calls without a final answer (raise --max-steps if this is expected)");
          return;
        }

        // 合批的出口。任何**非 delta** 帧之前都必须先调它（顺序即正确性）。
        const auto flush_delta = [&]() {
          if (pending.empty()) return;
          emit(proto::frame_event(++seq_, "assistant.delta",
                                  json{{"turn_id", turn_id}, {"step", step}, {"text", pending}}));
          pending.clear();
          last_flush = std::chrono::steady_clock::now();
        };

        // 进模型调用之前先看一眼：上一个 step 跑工具的时候，用户可能已经按过 Ctrl-C
        pump_input();
        if (cancel_requested_) {
          session_commit(turn, "cancelled", json::object(), step - 1,
                         static_cast<int64_t>(tool_calls_total), history_mark, compacted);
          emit(proto::frame_response_ok(id, json{{"turn_id", turn_id},
                                                 {"text", ""},
                                                 {"stop_reason", "cancelled"},
                                                 {"usage", json::object()},
                                                 {"steps", step - 1},
                                                 {"tool_calls", static_cast<int64_t>(tool_calls_total)}}));
          return;
        }

        // S6：进模型之前看一眼预算。压缩只动 history_（和本地那个回滚水位），协议只多一个事件。
        // 放在这里（而不是构造时）的理由：每多一个 step 历史都会变长，压不压要看"现在"有多少。
        const CompactReport rep = ctx_.compact(history_);
        if (rep.did) {
          compacted = true;
          // 历史可能被压得比回滚水位还短；不 clamp 的话 resize() 会把它**撑大**（补空消息）。
          if (history_mark > history_.size()) history_mark = history_.size();
          emit(proto::frame_event(++seq_, "context.compacted",
                                  json{{"turn_id", turn_id},
                                       {"step", step},
                                       {"budget", static_cast<int64_t>(ctx_.config().max_tokens)},
                                       {"before_tokens", static_cast<int64_t>(rep.before_tokens)},
                                       {"after_tokens", static_cast<int64_t>(rep.after_tokens)},
                                       {"dropped_messages", static_cast<int64_t>(rep.dropped_messages)},
                                       {"summarized_turns", static_cast<int64_t>(rep.summarized_turns)},
                                       {"truncated_tool_results", static_cast<int64_t>(rep.truncated_tools)}}));
        }

        ChatRequest req;
        req.messages = history_;
        req.tools = specs;

        // delta 带 step：一个 turn 可能有多个 step，前端要知道"这句话是哪一轮说的"。
        // 返回值 false = 取消：provider 会把它变成 content_receiver 的返回值，HTTP 客户端于是中止传输。
        const auto on_delta = [&](const std::string& piece) -> bool {
          // 顺序是有讲究的：
          // 1) 这一片**已经**被 provider 算进 result.text 了 —— 先收下（丢了就破不变量 2）；
          // 2) 顺手看一眼 stdin：这就是"一轮正在流的时候也能按 Ctrl-C"的全部实现；
          // 3) 该合批就合批；4) 最后才决定要不要继续读。
          pending += piece;
          pump_input();
          const auto now = std::chrono::steady_clock::now();
          const auto waited =
              std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count();
          if (opts_.delta_batch_ms <= 0 || pending.size() >= 512 ||
              waited >= static_cast<long long>(opts_.delta_batch_ms))
            flush_delta();
          // 返回值 false = 取消：provider 会把它变成 content_receiver 的返回值，HTTP 客户端于是中止传输。
          // 没 flush 的这批不会丢：调用返回后还有一次 flush_delta()。
          return !cancel_requested_;
        };

        const ChatResult r = use_stream ? provider_->chat_stream(req, on_delta) : provider_->chat(req);
        flush_delta();  // 先把最后一小批吐干净，再谈别的（不变量 2）

        if (r.cancelled || cancel_requested_) {
          // 取消 ≠ 失败。已经产出的文本**留在历史里**：用户屏幕上看到的和模型记得的必须一致，
          // 否则下一轮就是"半句话 + 两个连续的 user"，比留着更糟。
          // 代价写在 docs/stage-s5.md：取消时不做整轮回滚（那是失败语义）。
          if (!r.text.empty()) history_.push_back(Message{"assistant", r.text});
          session_commit(turn, "cancelled", r.usage, step, static_cast<int64_t>(tool_calls_total),
                         history_mark, compacted);
          emit(proto::frame_response_ok(id, json{{"turn_id", turn_id},
                                                 {"text", r.text},
                                                 {"stop_reason", "cancelled"},
                                                 {"usage", r.usage},
                                                 {"steps", step},
                                                 {"tool_calls", static_cast<int64_t>(tool_calls_total)}}));
          return;
        }

        if (r.tool_calls.empty()) {  // 模型给出最终答复：本 turn 结束
          history_.push_back(Message{"assistant", r.text});
          session_commit(turn, r.finish_reason, r.usage, step,
                         static_cast<int64_t>(tool_calls_total), history_mark, compacted);
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

          // S8：先过策略闸门；ask 时发 approval.requested，等前端回 approval.respond。
          bool allowed = true;
          std::string denial;
          if (arg_error.empty()) {
            const PolicyResult policy = policy_.check(call.name, args);
            if (policy.permission == Permission::Deny) {
              allowed = false;
              denial = policy.reason;
            } else if (policy.permission == Permission::Ask) {
              const std::string approval_id = "a" + std::to_string(++approval_counter_);
              json ask = {{"approval_id", approval_id},
                          {"turn_id", turn_id},
                          {"step", step},
                          {"call_id", call.id},
                          {"name", call.name},
                          {"arguments", args},
                          {"reason", policy.reason}};
              std::string why;
              allowed = request_approval(ask, why);
              if (!allowed) denial = "approval denied: " + why;
            }
          }

          ToolResult res;
          if (!allowed) {
            res = ToolResult::failure(denial);
          } else if (!arg_error.empty()) {
            res = ToolResult::failure(arg_error + ": " + truncate(call.arguments, 200));
          } else {
            res = tools_.execute(call.name, args);
          }
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
      session_abort(turn, "PROVIDER_ERROR", compacted);
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

int CoreEngine::run() {
  stdio_mode_ = true;
  sink_ = [](const std::string& frame) { std::cout << frame << std::flush; };

  // 第一帧永远是 event hello（protocol.md §4.1）
  greet(sink_);

  while (!shutdown_requested_) {
    // 一轮进行中被排队的请求：现在按到达顺序补上（协议：每个请求都必须有响应）
    if (!deferred_.empty()) {
      json v = std::move(deferred_.front());
      deferred_.erase(deferred_.begin());
      handle_request(v, sink_);
      continue;
    }
    std::string line;
    if (!read_next_line(line)) break;  // EOF：cli 关了管道
    if (line.empty()) continue;        // 忍耐空行，不打断会话
    dispatch_line(line);
  }

  std::cout << std::flush;  // 退出前必须 flush（protocol.md §4.3 铁律）
  return exit_code_;
}

}  // namespace core