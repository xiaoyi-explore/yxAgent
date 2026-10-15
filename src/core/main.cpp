// agent-core —— 大脑。S1 只会一件事：按 docs/protocol.md 应答。
#include "common/protocol.hpp"
#include <ctime>
#include <iostream>
#include <unistd.h>

using proto::json;  // 协议别名进入全局作用域（S1 的两个 main 都要用）

int main() {
  std::ios::sync_with_stdio(false);
  int64_t seq = 0;
  int exit_code = 0;

  // 第一帧永远是 event hello（protocol.md §4.1）
  std::cout << proto::frame_event(++seq, "hello", json{
      {"protocol_version", proto::PROTOCOL_VERSION},
      {"core_version", proto::CORE_VERSION},
      {"pid", (int64_t)getpid()}})
            << std::flush;

  const auto send_err = [&](const json& id, const std::string& code, const std::string& msg) {
    std::cout << proto::frame_response_err(id, code, msg) << std::flush;
  };

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;  // 忍耐空行，不打断会话

    json req = proto::parse_frame(line);
    if (req.is_null()) {
      send_err(json(), "PARSE_ERROR", "frame is not a valid JSON object");
      continue;
    }
    // protocol.md §6 未知即忽略：event / response / 未知 type 一律丢弃，绝不响应
    const std::string ftype = req.contains("type") && req["type"].is_string()
                            ? req["type"].get<std::string>() : "";
    if (ftype != "request") continue;
    if (!proto::is_request(req)) {
      send_err(proto::extract_id(req), "INVALID_ENVELOPE",
               "expected a request envelope (type=request with string id and method)");
      continue;
    }

    const json& id = req["id"];
    const std::string method = req["method"].get<std::string>();
    json params = (req.contains("params") && req["params"].is_object()) ? req["params"] : json::object();

    if (method == "hello") {
      const std::string pv = params.contains("protocol_version") && params["protocol_version"].is_string()
                           ? params["protocol_version"].get<std::string>() : "";
      if (proto::major_of(pv) != proto::major_of(proto::PROTOCOL_VERSION)) {
        send_err(id, "PROTOCOL_VERSION_MISMATCH",
                 "core speaks " + std::string(proto::PROTOCOL_VERSION) + ", client sent " + pv);
        exit_code = 2;  // protocol.md §4.2：major 不兼容 -> core 退出码 2
        break;
      }
      std::cout << proto::frame_response_ok(id, json{
          {"protocol_version", proto::PROTOCOL_VERSION},
          {"core_version", proto::CORE_VERSION}}) << std::flush;
    } else if (method == "ping") {
      json result = json::object();
      if (params.contains("client_ts")) result["client_ts"] = params["client_ts"];
      result["core_ts"] = (int64_t)time(nullptr);
      std::cout << proto::frame_response_ok(id, result) << std::flush;
    } else if (method == "echo") {
      std::cout << proto::frame_response_ok(id, params) << std::flush;
    } else if (method == "shutdown") {
      std::cout << proto::frame_response_ok(id, json{{"will_exit", true}}) << std::flush;
      break;
    } else {
      send_err(id, "UNKNOWN_METHOD", "no such method: " + method);
    }
  }

  // 退出前必须 flush（protocol.md §4.3 铁律）
  std::cout << std::flush;
  return exit_code;
}
