#pragma once
// agent_common —— 两个进程共同遵守的协议工具。
// 铁律（docs/roadmap.md 第三章）：本库绝不链接 HTTP 库或 TUI 库。
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>

namespace proto {

using json = nlohmann::json;

inline constexpr const char* PROTOCOL_VERSION = "1.0";
inline constexpr const char* CORE_VERSION = "0.0.1";

// 单帧上限（docs/protocol.md §1）
inline constexpr size_t MAX_FRAME_BYTES = 1u << 20;

// ---- 构帧：四个工厂函数，对应三种信封 ----

inline std::string frame_request(const std::string& id, const std::string& method, json params) {
  json r = {{"type","request"},{"id",id},{"method",method},{"params",std::move(params)}};
  return r.dump() + "\n";
}

inline std::string frame_event(int64_t seq, const std::string& name, json payload) {
  json e = {{"type","event"},{"seq",seq},{"name",name},{"payload",std::move(payload)}};
  return e.dump() + "\n";
}

inline std::string frame_response_ok(const json& id, json result) {
  json r = {{"type","response"},{"id",id},{"ok",true},{"result",std::move(result)}};
  return r.dump() + "\n";
}

inline std::string frame_response_err(const json& id, const std::string& code, const std::string& message) {
  json r = {{"type","response"},{"id",id},{"ok",false},{"error",{{"code",code},{"message",message}}}};
  return r.dump() + "\n";
}

// ---- 解帧：一行 -> json；失败返回 null json，绝不抛出 ----

inline json parse_frame(const std::string& line) {
  if (line.size() > MAX_FRAME_BYTES) return json();
  try {
    json v = json::parse(line);
    if (v.is_object()) return v;
  } catch (...) {}
  return json();
}

inline bool is_request(const json& v) {
  return v.contains("type") && v["type"] == "request"
      && v.contains("id") && v["id"].is_string()
      && v.contains("method") && v["method"].is_string();
}

// 尽量从坏帧里抠出 id（protocol.md §5：请求不能悬空）
inline json extract_id(const json& v) {
  if (v.is_object() && v.contains("id") && v["id"].is_string()) return v["id"];
  return json();  // null
}

// major 相同即兼容（protocol.md §4.2）
inline std::string major_of(const std::string& version) {
  auto pos = version.find('.');
  return pos == std::string::npos ? version : version.substr(0, pos);
}

}  // namespace proto
