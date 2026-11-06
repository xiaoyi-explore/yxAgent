#pragma once
// TuiModel（S9）—— TUI 的"视图状态"，与 FTXUI 完全解耦。
//
// 它只消费协议帧，产出可渲染的行 + 状态栏 + 当前审批请求。
// 这样 TUI 的交互逻辑可以在没有终端的单元测试里穷举；FTXUI 只负责画。
#include "common/protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace core {

using proto::json;

struct TuiLine {
  std::string kind;  // user / assistant / tool / approval / error / system
  std::string text;
};

class TuiModel {
 public:
  void set_session(const json& session);

  void begin_turn(const std::string& prompt);
  void on_event(const json& event);
  void on_response(const json& response);
  void on_approval_decided(const std::string& decision);
  void on_cancel_requested();
  void on_core_died(const std::string& why);

  bool busy() const { return busy_; }
  bool approval_pending() const { return approval_pending_; }
  const json& approval() const { return approval_; }
  const std::vector<TuiLine>& lines() const { return lines_; }
  std::string status() const { return status_; }
  std::string session_id() const { return session_id_; }
  int64_t turns() const { return turns_; }

 private:
  void push(std::string kind, std::string text);
  void set_status(std::string text) { status_ = std::move(text); }

  std::vector<TuiLine> lines_;
  bool busy_ = false;
  bool approval_pending_ = false;
  json approval_ = json::object();
  std::string status_ = "idle";
  std::string session_id_;
  int64_t turns_ = 0;
  int64_t current_step_ = 0;
  size_t current_assistant_ = 0;
  size_t deltas_in_last_step_ = 0;
};

}  // namespace core