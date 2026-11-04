#pragma once
// SessionStore（S7）—— 会话落盘与恢复。
//
// 设计（见 docs/stage-s7.md）：
//   * 文件是 append-only NDJSON：session.* 记录是恢复真相，request/response/event
//     帧只做审计与 seq 水位；
//   * 一轮一条 session.commit（原子提交），失败轮只有 session.abort，不写消息；
//   * 压缩改写 history_ 时写 session.checkpoint（全量消息），append-only 文件里的重置点；
//   * 每会话单写者：flock(LOCK_EX|LOCK_NB)，拿不到就是 SESSION_BUSY。
//
// 本文件只认 Message / json，不认识 HTTP、工具、协议循环。
#include "common/protocol.hpp"
#include "core/provider.hpp"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace core {

struct SessionError : std::runtime_error {
  std::string code;
  SessionError(std::string c, const std::string& message)
      : std::runtime_error(message), code(std::move(c)) {}
};

struct SessionHeaderConfig {
  std::string model;
  std::string workdir;
  std::vector<std::string> tools;
};

struct SessionOpenResult {
  std::string id;
  std::string path;
  bool created = false;
  bool resumed = false;
  int64_t turns = 0;
  int64_t last_seq = 0;
  std::vector<Message> messages;
};

// ~/.agent/sessions（HOME 缺失时退回当前目录下的 .agent/sessions）
std::string default_session_dir();

class SessionStore {
 public:
  explicit SessionStore(std::string dir);
  ~SessionStore();

  SessionStore(const SessionStore&) = delete;
  SessionStore& operator=(const SessionStore&) = delete;

  // 打开或新建。create=false 且文件不存在 -> SESSION_NOT_FOUND。
  SessionOpenResult open(const std::string& id, bool create,
                         const std::vector<Message>& initial_messages,
                         const SessionHeaderConfig& cfg);
  // 最近使用/创建的会话；没有 -> SESSION_NOT_FOUND。
  SessionOpenResult open_latest(const std::vector<Message>& initial_messages,
                                const SessionHeaderConfig& cfg);

  // 审计帧：只接受 request/response/event；session.load 请求不落盘。
  void append_frame(const json& frame);
  void append_session_open(bool resumed);
  void append_turn_begin(int64_t turn);
  void append_commit(int64_t turn, const std::string& stop_reason, const json& usage,
                     int64_t steps, int64_t tool_calls,
                     const std::vector<Message>& messages,
                     const std::vector<Message>* checkpoint);
  void append_abort(int64_t turn, const std::string& code,
                    const std::vector<Message>* checkpoint);

  const std::string& id() const { return id_; }
  const std::string& path() const { return path_; }
  bool created() const { return created_; }
  bool resumed() const { return resumed_; }
  int64_t last_seq() const { return last_seq_; }
  int64_t last_turn() const { return last_turn_; }
  json info(bool with_path) const;

  static bool valid_id(const std::string& id);
  static std::string make_id();

 private:
  void ensure_dir();
  void open_file(const std::string& path);
  void close_file();
  void write_record(const json& record, bool sync);
  void write_header(const SessionHeaderConfig& cfg);
  void write_last_pointer();
  void load_file(const std::string& path, SessionOpenResult& out);

  std::string dir_;
  std::string id_;
  std::string path_;
  std::FILE* file_ = nullptr;
  bool created_ = false;
  bool resumed_ = false;
  int64_t last_seq_ = 0;
  int64_t last_turn_ = 0;
};

}  // namespace core
