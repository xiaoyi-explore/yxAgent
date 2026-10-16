#include "core/core_engine.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
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
    else if ((c & 0xF8) == 0xF0) len = 4;
    else                         len = 1;  // 非法起始字节：原样单字节吐，绝不卡死
    if (i + len > s.size())      len = 1;  // 被截断的序列：同样单字节吐
    out.push_back(s.substr(i, len));
    i += len;
  }
  return out;
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
