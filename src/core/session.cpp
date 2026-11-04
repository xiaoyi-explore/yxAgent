#include "core/session.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace core {
namespace {

namespace fs = std::filesystem;

int64_t now_seconds() { return static_cast<int64_t>(std::time(nullptr)); }

std::string str_field(const json& j, const char* key) {
  const auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

Message message_from_json(const json& j) {
  Message m;
  m.role = str_field(j, "role");
  m.content = str_field(j, "content");
  const auto tc = j.find("tool_calls");
  if (tc != j.end() && tc->is_array()) {
    for (const json& item : *tc) {
      ToolCall call;
      call.id = str_field(item, "id");
      call.name = str_field(item, "name");
      call.arguments = str_field(item, "arguments");
      m.tool_calls.push_back(std::move(call));
    }
  }
  m.tool_call_id = str_field(j, "tool_call_id");
  m.name = str_field(j, "name");
  return m;
}

json message_to_json(const Message& m) {
  json j = {{"role", m.role}, {"content", m.content}};
  if (!m.tool_calls.empty()) {
    json calls = json::array();
    for (const ToolCall& c : m.tool_calls)
      calls.push_back(json{{"id", c.id}, {"name", c.name}, {"arguments", c.arguments}});
    j["tool_calls"] = std::move(calls);
  }
  if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
  if (!m.name.empty()) j["name"] = m.name;
  return j;
}

std::vector<Message> messages_from_json(const json& value) {
  if (!value.is_array())
    throw SessionError("SESSION_CORRUPT", "session record has a non-array messages field");
  std::vector<Message> out;
  out.reserve(value.size());
  for (const json& item : value) {
    if (!item.is_object())
      throw SessionError("SESSION_CORRUPT", "session record contains a non-object message");
    out.push_back(message_from_json(item));
  }
  return out;
}

json messages_to_json(const std::vector<Message>& messages) {
  json out = json::array();
  for (const Message& m : messages) out.push_back(message_to_json(m));
  return out;
}

std::string read_last_id(const std::string& dir) {
  std::ifstream in(fs::path(dir) / "last");
  std::string id;
  if (!std::getline(in, id)) return {};
  while (!id.empty() && (id.back() == '\r' || id.back() == '\n')) id.pop_back();
  return id;
}

}  // namespace

std::string default_session_dir() {
  const char* home = std::getenv("HOME");
  const std::string base = (home != nullptr && *home != '\0') ? home : ".";
  return (fs::path(base) / ".agent" / "sessions").string();
}

bool SessionStore::valid_id(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  if (id.find("..") != std::string::npos) return false;
  const auto alnum = [](unsigned char c) { return std::isalnum(c) != 0; };
  if (!alnum(static_cast<unsigned char>(id.front()))) return false;
  for (unsigned char c : id) {
    if (alnum(c) || c == '.' || c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

std::string SessionStore::make_id() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char stamp[32] = {0};
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  std::random_device rd;
  char suffix[8] = {0};
  std::snprintf(suffix, sizeof(suffix), "%04x", static_cast<unsigned>(rd() & 0xffffu));
  return std::string(stamp) + "-" + suffix;
}

SessionStore::SessionStore(std::string dir) : dir_(std::move(dir)) {}
SessionStore::~SessionStore() { close_file(); }

void SessionStore::ensure_dir() {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  if (ec)
    throw SessionError("SESSION_IO_ERROR", "cannot create session directory " + dir_ + ": " + ec.message());
  ::chmod(dir_.c_str(), 0700);
}

void SessionStore::open_file(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0600);
  if (fd < 0)
    throw SessionError("SESSION_IO_ERROR", "cannot open session file " + path + ": " + std::strerror(errno));
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int e = errno;
    ::close(fd);
    if (e == EWOULDBLOCK || e == EAGAIN)
      throw SessionError("SESSION_BUSY", "session is already open by another process: " + path);
    throw SessionError("SESSION_IO_ERROR", "cannot lock session file " + path + ": " + std::strerror(e));
  }
  file_ = ::fdopen(fd, "a");
  if (file_ == nullptr) {
    ::close(fd);
    throw SessionError("SESSION_IO_ERROR", "cannot open session stream " + path);
  }
  ::chmod(path.c_str(), 0600);
}

void SessionStore::close_file() {
  if (file_ != nullptr) {
    std::fclose(file_);  // flock 随 fd 关闭释放
    file_ = nullptr;
  }
}

void SessionStore::write_record(const json& record, bool sync) {
  if (file_ == nullptr)
    throw SessionError("SESSION_IO_ERROR", "session file is not open");
  std::string line = record.dump();
  line.push_back('\n');
  if (std::fwrite(line.data(), 1, line.size(), file_) != line.size())
    throw SessionError("SESSION_IO_ERROR", "cannot write session file " + path_);
  if (std::fflush(file_) != 0)
    throw SessionError("SESSION_IO_ERROR", "cannot flush session file " + path_);
  if (sync && ::fsync(::fileno(file_)) != 0)
    throw SessionError("SESSION_IO_ERROR", "cannot fsync session file " + path_);
}

void SessionStore::write_header(const SessionHeaderConfig& cfg) {
  json header = {{"type", "session.header"},
                 {"v", 1},
                 {"id", id_},
                 {"created_at", now_seconds()},
                 {"protocol_version", proto::PROTOCOL_VERSION},
                 {"core_version", proto::CORE_VERSION},
                 {"model", cfg.model},
                 {"tools", cfg.tools},
                 {"workdir", cfg.workdir}};
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec) header["cwd"] = cwd.string();
  write_record(header, true);
}

void SessionStore::write_last_pointer() {
  const std::string last = (fs::path(dir_) / "last").string();
  const std::string tmp = last + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << id_ << "\n";
    if (!out) {
      std::cerr << "[session] warning: cannot write " << last << "\n";
      return;
    }
  }
  ::chmod(tmp.c_str(), 0600);
  if (::rename(tmp.c_str(), last.c_str()) != 0) {
    std::cerr << "[session] warning: cannot publish " << last << ": " << std::strerror(errno) << "\n";
    ::remove(tmp.c_str());
    return;
  }
  ::chmod(last.c_str(), 0600);
}

SessionOpenResult SessionStore::open(const std::string& id, bool create,
                                     const std::vector<Message>& initial_messages,
                                     const SessionHeaderConfig& cfg) {
  if (!valid_id(id))
    throw SessionError("INVALID_PARAMS",
                       "invalid session id '" + id + "' (use letters, digits, '.', '_' or '-')");

  ensure_dir();
  path_ = (fs::path(dir_) / (id + ".jsonl")).string();
  std::error_code ec;
  const bool exists = fs::exists(path_, ec);
  const uintmax_t size = exists ? fs::file_size(path_, ec) : 0;

  SessionOpenResult out;
  if (exists && size > 0) {
    id_ = id;
    created_ = false;
    resumed_ = true;
    last_seq_ = 0;
    last_turn_ = 0;
    open_file(path_);
    load_file(path_, out);
    out.id = id;
    out.path = path_;
    out.created = false;
    out.resumed = true;
    out.turns = last_turn_;
    out.last_seq = last_seq_;
    write_last_pointer();
    return out;
  }

  if (!create) {
    if (exists)
      throw SessionError("SESSION_CORRUPT", "session file is empty: " + path_);
    throw SessionError("SESSION_NOT_FOUND",
                       "no session '" + id + "' under " + dir_ + "; start one with --session " + id);
  }

  id_ = id;
  created_ = true;
  resumed_ = false;
  last_seq_ = 0;
  last_turn_ = 0;
  open_file(path_);
  write_header(cfg);
  write_record(json{{"type", "session.checkpoint"},
                    {"turn", 0},
                    {"reason", "start"},
                    {"at", now_seconds()},
                    {"messages", messages_to_json(initial_messages)}},
               true);
  write_last_pointer();

  out.id = id;
  out.path = path_;
  out.created = true;
  out.resumed = false;
  out.turns = 0;
  out.last_seq = 0;
  out.messages = initial_messages;
  return out;
}

SessionOpenResult SessionStore::open_latest(const std::vector<Message>& initial_messages,
                                            const SessionHeaderConfig& cfg) {
  ensure_dir();

  const std::string last = read_last_id(dir_);
  if (!last.empty() && valid_id(last)) {
    const fs::path candidate = fs::path(dir_) / (last + ".jsonl");
    std::error_code ec;
    if (fs::exists(candidate, ec)) return open(last, false, initial_messages, cfg);
  }

  std::string best;
  fs::file_time_type best_time{};
  std::error_code ec;
  for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string name = it->path().filename().string();
    if (name.size() < 6 || name.substr(name.size() - 6) != ".jsonl") continue;
    const std::string id = name.substr(0, name.size() - 6);
    if (!valid_id(id)) continue;
    const auto t = it->last_write_time(ec);
    if (best.empty() || t > best_time) {
      best = id;
      best_time = t;
    }
  }
  if (best.empty())
    throw SessionError("SESSION_NOT_FOUND",
                       "no sessions under " + dir_ + "; start one with --session <id>");
  return open(best, false, initial_messages, cfg);
}

void SessionStore::load_file(const std::string& path, SessionOpenResult& out) {
  std::ifstream in(path);
  if (!in)
    throw SessionError("SESSION_IO_ERROR", "cannot read session file " + path);

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(std::move(line));
    line.clear();
  }
  if (lines.empty())
    throw SessionError("SESSION_CORRUPT", "session file has no header: " + path);

  const json header = json::parse(lines.front(), nullptr, false);
  if (header.is_discarded() || !header.is_object() || str_field(header, "type") != "session.header")
    throw SessionError("SESSION_CORRUPT", "session file does not start with a session.header: " + path);
  const int version = header.value("v", 0);
  if (version != 1)
    throw SessionError("SESSION_UNSUPPORTED_VERSION",
                       "session format v" + std::to_string(version) + " is not supported by this core (" +
                           path + ")");

  std::vector<Message> messages;
  int64_t last_turn = 0;
  int64_t last_seq = 0;

  for (size_t i = 1; i < lines.size(); ++i) {
    json rec = json::parse(lines[i], nullptr, false);
    if (rec.is_discarded() || !rec.is_object()) {
      if (i + 1 == lines.size()) {
        std::cerr << "[session] warning: ignoring truncated last line in " << path << "\n";
        break;
      }
      throw SessionError("SESSION_CORRUPT", "malformed record at " + path + ":" + std::to_string(i + 1));
    }
    const std::string type = str_field(rec, "type");
    if (type == "session.checkpoint") {
      messages = messages_from_json(rec.value("messages", json::array()));
      last_turn = std::max(last_turn, rec.value("turn", static_cast<int64_t>(0)));
    } else if (type == "session.commit") {
      if (rec.contains("checkpoint"))
        messages = messages_from_json(rec["checkpoint"]);
      else {
        const std::vector<Message> added = messages_from_json(rec.value("messages", json::array()));
        messages.insert(messages.end(), added.begin(), added.end());
      }
      last_turn = std::max(last_turn, rec.value("turn", static_cast<int64_t>(0)));
    } else if (type == "session.abort") {
      if (rec.contains("checkpoint")) messages = messages_from_json(rec["checkpoint"]);
      last_turn = std::max(last_turn, rec.value("turn", static_cast<int64_t>(0)));
    } else if (type == "session.turn.begin") {
      last_turn = std::max(last_turn, rec.value("turn", static_cast<int64_t>(0)));
    } else if (type == "event") {
      const int64_t seq = rec.value("seq", static_cast<int64_t>(0));
      if (seq <= last_seq)
        throw SessionError("SESSION_CORRUPT",
                           "event seq is not strictly increasing at " + path + ":" +
                               std::to_string(i + 1) + " (seq " + std::to_string(seq) + ")");
      last_seq = seq;
    }
  }

  out.messages = std::move(messages);
  last_turn_ = last_turn;
  last_seq_ = last_seq;
}

void SessionStore::append_frame(const json& frame) {
  if (file_ == nullptr) return;
  const std::string type = str_field(frame, "type");
  if (type == "request") {
    if (str_field(frame, "method") == "session.load") return;  // 会话切换请求不写进任何一边
  } else if (type != "event" && type != "response") {
    return;
  }
  write_record(frame, false);
  if (type == "event") last_seq_ = std::max(last_seq_, frame.value("seq", static_cast<int64_t>(0)));
}

void SessionStore::append_session_open(bool resumed) {
  if (file_ == nullptr) return;
  write_record(json{{"type", "session.open"},
                    {"resumed", resumed},
                    {"pid", static_cast<int64_t>(::getpid())},
                    {"at", now_seconds()}},
               false);
}

void SessionStore::append_turn_begin(int64_t turn) {
  if (file_ == nullptr) return;
  write_record(json{{"type", "session.turn.begin"}, {"turn", turn}, {"at", now_seconds()}}, false);
  last_turn_ = std::max(last_turn_, turn);
}

void SessionStore::append_commit(int64_t turn, const std::string& stop_reason, const json& usage,
                                 int64_t steps, int64_t tool_calls,
                                 const std::vector<Message>& messages,
                                 const std::vector<Message>* checkpoint) {
  if (file_ == nullptr) return;
  json rec = {{"type", "session.commit"},
              {"turn", turn},
              {"at", now_seconds()},
              {"stop_reason", stop_reason},
              {"usage", usage},
              {"steps", steps},
              {"tool_calls", tool_calls}};
  if (checkpoint != nullptr)
    rec["checkpoint"] = messages_to_json(*checkpoint);
  else
    rec["messages"] = messages_to_json(messages);
  write_record(rec, true);
  last_turn_ = std::max(last_turn_, turn);
}

void SessionStore::append_abort(int64_t turn, const std::string& code,
                                const std::vector<Message>* checkpoint) {
  if (file_ == nullptr) return;
  json rec = {{"type", "session.abort"}, {"turn", turn}, {"at", now_seconds()}, {"code", code}};
  if (checkpoint != nullptr) rec["checkpoint"] = messages_to_json(*checkpoint);
  write_record(rec, true);
  last_turn_ = std::max(last_turn_, turn);
}

json SessionStore::info(bool with_path) const {
  json j = {{"id", id_},
            {"resumed", resumed_},
            {"turns", last_turn_},
            {"last_seq", last_seq_}};
  if (with_path) j["path"] = path_;
  return j;
}

}  // namespace core