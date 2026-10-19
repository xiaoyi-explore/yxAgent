#pragma once
// SSE（text/event-stream）增量解析器 —— S3b 引入。
//
// 它只做一件事：把"任意切分的字节流"还原成"完整的事件"。
// 算法（出问题就按这四条排查，new-agent-stages-v2.md S3b 原文）：
//   1. 每个 chunk 追加进 buffer，**绝不假设 chunk 落在事件边界**；
//   2. 只在看见**一整行**（\n / \r\n / \r）之后才处理这一行——
//      buffer 末尾的半行（包括只收到了 \r、还不知道后面跟不跟 \n 的情况）留到下一段；
//   3. `: ` 开头是注释（OpenAI 的 keep-alive ping），整行忽略；
//      多个 `data:` 行用 '\n' 拼接（SSE 规范）；
//   4. 空行 = 派发一个事件；派发后 erase 已消费的前缀，**不是 clear 整个 buffer**。
//
// 它不认识 HTTP，也不认识 OpenAI：sse.cpp 不 include 任何网络库（铁律见 responsibilities.md）。
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace core {

struct SseEvent {
  std::string event;  // `event:` 字段；空表示默认事件 `message`
  std::string data;   // 所有 `data:` 行用 '\n' 拼起来，末尾不带换行
};

class SseParser {
 public:
  using Handler = std::function<void(const SseEvent&)>;

  explicit SseParser(Handler on_event, size_t max_buffer = 8u << 20)
      : on_event_(std::move(on_event)), max_buffer_(max_buffer) {}

  // 喂一段原始字节；返回本次新派发的事件数。可以喂任意切分（1 字节也行）。
  size_t feed(const char* data, size_t len);
  size_t feed(const std::string& chunk) { return feed(chunk.data(), chunk.size()); }

  // 流结束（对端关闭 / 读到 EOF）：把最后一个没有被空行收尾的事件也派发掉。
  size_t flush();

  size_t dispatched() const { return dispatched_; }
  size_t buffered() const { return buf_.size(); }

 private:
  // 从 buffer 头部取一整行：返回消费的字节数，0 = 还没凑够一整行。
  size_t next_line(std::string& line) const;
  size_t handle_line(const std::string& line);
  size_t dispatch();

  Handler on_event_;
  size_t max_buffer_;
  std::string buf_;    // 还没消费的字节（可能停在任何位置，含半个 \r\n）
  std::string data_;   // 当前事件的 data（多行已拼接）
  std::string event_;  // 当前事件的 event 名
  bool data_seen_ = false;
  size_t dispatched_ = 0;
};

}  // namespace core
