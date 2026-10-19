#include "core/sse.hpp"

#include <stdexcept>
#include <utility>

namespace core {

size_t SseParser::next_line(std::string& line) const {
  for (size_t i = 0; i < buf_.size(); ++i) {
    if (buf_[i] == '\n') {
      line.assign(buf_, 0, i);
      return i + 1;
    }
    if (buf_[i] == '\r') {
      // \r 可能是 \r\n 的前半截：等下一段来了再决定。流真结束时由 flush() 收下。
      if (i + 1 >= buf_.size()) return 0;
      line.assign(buf_, 0, i);
      return (buf_[i + 1] == '\n') ? i + 2 : i + 1;
    }
  }
  return 0;  // 没有行尾：这一行还没收完
}

size_t SseParser::feed(const char* data, size_t len) {
  if (data == nullptr || len == 0) return 0;
  buf_.append(data, len);
  if (buf_.size() > max_buffer_) {
    buf_.clear();
    throw std::runtime_error("SSE: more than " + std::to_string(max_buffer_) +
                             " bytes buffered without a complete line");
  }

  size_t n = 0;
  std::string line;
  for (;;) {
    const size_t consumed = next_line(line);
    if (consumed == 0) break;          // 剩半行：留着，等下一个 chunk
    buf_.erase(0, consumed);           // 只丢已消费的前缀，不 clear 整个 buffer
    n += handle_line(line);
  }
  return n;
}

size_t SseParser::flush() {
  size_t n = 0;
  if (!buf_.empty()) {
    std::string line = std::move(buf_);
    buf_.clear();
    if (!line.empty() && line.back() == '\r') line.pop_back();  // 结尾的孤立 \r
    n += handle_line(line);
  }
  n += dispatch();  // 最后一个事件可能没有以空行收尾（对端直接断在事件中间）
  return n;
}

size_t SseParser::handle_line(const std::string& line) {
  if (line.empty()) return dispatch();  // 空行 = 一个事件的边界
  if (line[0] == ':') return 0;         // 注释 / keep-alive：整行丢掉（不计入 data）

  const size_t colon = line.find(':');
  std::string field;
  std::string value;
  if (colon == std::string::npos) {
    field = line;  // 只有字段名，没有值的行（规范允许）
  } else {
    field = line.substr(0, colon);
    value = line.substr(colon + 1);
    if (!value.empty() && value[0] == ' ') value.erase(0, 1);  // 规范：只吃掉一个空格
  }

  if (field == "event") {
    event_ = value;
  } else if (field == "data") {
    if (data_seen_) data_ += '\n';  // 多行 data 用换行拼接
    data_ += value;
    data_seen_ = true;
  }
  // `id` / `retry` / 未知字段：忽略（protocol.md §6 未知即忽略）
  return 0;
}

size_t SseParser::dispatch() {
  if (!data_seen_ && event_.empty()) return 0;  // 心跳空行：没有事件可派发

  SseEvent ev;
  ev.event = std::move(event_);
  ev.data = std::move(data_);
  event_.clear();
  data_.clear();
  data_seen_ = false;
  ++dispatched_;
  if (on_event_) on_event_(ev);
  return 1;
}

}  // namespace core
