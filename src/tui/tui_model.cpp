#include "tui/tui_model.hpp"

#include <utility>

namespace core {

void TuiModel::push(std::string kind, std::string text) {
  lines_.push_back(TuiLine{std::move(kind), std::move(text)});
}

void TuiModel::set_session(const json& session) {
  if (session.is_object()) {
    session_id_ = session.value("id", "");
    turns_ = session.value("turns", static_cast<int64_t>(0));
    set_status(session_id_.empty() ? "idle" : "session " + session_id_);
  }
}

void TuiModel::begin_turn(const std::string& prompt) {
  push("user", "> " + prompt);
  busy_ = true;
  approval_pending_ = false;
  approval_ = json::object();
  current_step_ = 0;
  current_assistant_ = 0;
  deltas_in_last_step_ = 0;
  set_status("thinking...");
}

void TuiModel::on_event(const json& event) {
  const std::string name = event.value("name", "");
  const json payload = event.contains("payload") ? event["payload"] : json::object();

  if (name == "assistant.delta") {
    const int64_t step = payload.value("step", static_cast<int64_t>(1));
    if (step != current_step_ || lines_.empty() ||
        lines_[current_assistant_].kind != "assistant") {
      push("assistant", "");
      current_assistant_ = lines_.size() - 1;
      current_step_ = step;
      deltas_in_last_step_ = 0;
    }
    lines_[current_assistant_].text += payload.value("text", "");
    ++deltas_in_last_step_;
    set_status("streaming (step " + std::to_string(step) + ")");
  } else if (name == "tool.call") {
    const std::string tool = payload.value("name", "?");
    push("tool", "[tool] " + tool + " " +
                     payload.value("arguments", json::object()).dump());
    set_status("tool: " + tool);
  } else if (name == "tool.result") {
    const bool ok = payload.value("ok", false);
    std::string line = ok ? "  -> ok" : "  -> error (" + payload.value("error", "") + ")";
    push("tool", line);
    set_status(ok ? "tool done" : "tool failed");
  } else if (name == "context.compacted") {
    push("system", "[context] compacted ~" + std::to_string(payload.value("before_tokens", 0)) +
                       " -> ~" + std::to_string(payload.value("after_tokens", 0)));
    set_status("context compacted");
  } else if (name == "approval.requested") {
    approval_ = payload;
    approval_pending_ = true;
    push("approval", "[approval] " + payload.value("name", "?") + " " +
                          payload.value("arguments", json::object()).dump());
    set_status("approval pending");
  }
}

void TuiModel::on_response(const json& response) {
  busy_ = false;
  approval_pending_ = false;
  approval_ = json::object();
  ++turns_;

  if (!response.value("ok", false)) {
    const json err = response.contains("error") ? response["error"] : json::object();
    push("error", "[error] " + err.value("code", "?") + ": " + err.value("message", "?"));
    set_status("error");
    return;
  }

  const json result = response.contains("result") ? response["result"] : json::object();
  const std::string text = result.value("text", "");
  // 流式时最后一条 assistant 已经等于 result.text；无流式/最后一步没吐 delta 时才补播。
  if (!text.empty()) {
    const bool already = !lines_.empty() && lines_.back().kind == "assistant" &&
                         lines_.back().text == text;
    if (!already) push("assistant", text);
  }

  const std::string stop = result.value("stop_reason", "stop");
  set_status("idle (" + stop + ")");
  current_step_ = 0;
  deltas_in_last_step_ = 0;
}

void TuiModel::on_approval_decided(const std::string& decision) {
  approval_pending_ = false;
  approval_ = json::object();
  push("approval", "[approval] " + decision);
  set_status(decision == "allow" ? "approved" : "denied");
}

void TuiModel::on_cancel_requested() {
  set_status("cancelling...");
}

void TuiModel::on_core_died(const std::string& why) {
  busy_ = false;
  approval_pending_ = false;
  push("error", "[core died] " + why);
  set_status("core died");
}

}  // namespace core