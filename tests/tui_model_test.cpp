#include "tui/tui_model.hpp"

#include <iostream>
#include <string>

using core::TuiModel;
using proto::json;

namespace {
int fails = 0;
void check(const char* name, bool cond, const std::string& detail = "") {
  std::cout << (cond ? "PASS " : "FAIL ") << name;
  if (!cond && !detail.empty()) std::cout << "   " << detail;
  std::cout << "\n";
  if (!cond) ++fails;
}
}  // namespace

int main() {
  TuiModel m;
  m.set_session(json{{"id", "s1"}, {"turns", 2}});
  check("session id", m.session_id() == "s1", m.session_id());
  check("session turns", m.turns() == 2, std::to_string(m.turns()));
  check("初始 idle", !m.busy() && !m.approval_pending());

  m.begin_turn("你好");
  check("begin_turn 加 user", !m.lines().empty() && m.lines().back().kind == "user" &&
                                  m.lines().back().text.find("你好") != std::string::npos);
  check("begin_turn busy", m.busy());

  m.on_event(json{{"name", "assistant.delta"},
                  {"payload", {{"step", 1}, {"text", "你"}}}});
  m.on_event(json{{"name", "assistant.delta"},
                  {"payload", {{"step", 1}, {"text", "好"}}}});
  check("delta 合并到一条 assistant", m.lines().back().kind == "assistant" &&
                                          m.lines().back().text == "你好",
        m.lines().back().text);

  m.on_event(json{{"name", "tool.call"},
                  {"payload", {{"name", "read_file"}, {"arguments", {{"path", "a.txt"}}}}}});
  check("tool.call 可见", m.lines().back().kind == "tool" &&
                              m.lines().back().text.find("read_file") != std::string::npos);
  m.on_event(json{{"name", "tool.result"},
                  {"payload", {{"ok", false}, {"error", "boom"}}}});
  check("tool.result error", m.lines().back().kind == "tool" &&
                                 m.lines().back().text.find("boom") != std::string::npos);

  m.on_event(json{{"name", "context.compacted"},
                  {"payload", {{"before_tokens", 100}, {"after_tokens", 50}}}});
  check("compaction 可见", m.lines().back().kind == "system" &&
                               m.lines().back().text.find("compacted") != std::string::npos);

  m.on_event(json{{"name", "approval.requested"},
                  {"payload", {{"approval_id", "a1"}, {"name", "run_shell"},
                               {"arguments", {{"command", "rm -rf /tmp/x"}}},
                               {"reason", "dangerous"}}}});
  check("approval pending", m.approval_pending());
  check("approval payload", m.approval().value("approval_id", "") == "a1");
  m.on_approval_decided("deny");
  check("approval decided", !m.approval_pending() &&
                                m.lines().back().text.find("deny") != std::string::npos);

  m.on_response(json{{"ok", true},
                     {"result", {{"text", "你" + std::string("好")}, {"stop_reason", "stop"}}}});
  check("response 结束 busy", !m.busy());
  check("response 后 turns 增加", m.turns() == 3, std::to_string(m.turns()));
  check("已有 delta 不重复播全文", m.lines().back().text == "你好" ||
                                      m.lines().back().kind != "assistant",
        m.lines().back().text);

  TuiModel n;
  n.begin_turn("无流式");
  n.on_response(json{{"ok", true},
                     {"result", {{"text", "整段回答"}, {"stop_reason", "stop"}}}});
  check("无 delta 时播 result.text", n.lines().back().kind == "assistant" &&
                                        n.lines().back().text == "整段回答");

  TuiModel e;
  e.begin_turn("失败");
  e.on_response(json{{"ok", false},
                     {"error", {{"code", "PROVIDER_ERROR"}, {"message", "boom"}}}});
  check("错误可见", e.lines().back().kind == "error" &&
                        e.lines().back().text.find("PROVIDER_ERROR") != std::string::npos);
  check("错误后不再 busy", !e.busy());

  return fails == 0 ? 0 : 1;
}
