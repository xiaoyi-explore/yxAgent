#pragma once
// Provider —— core 与"某个模型服务"之间的接缝。
// S3a 只有非流式（一次拿到整段回复）；S3b 在同一接缝上长出 chat_stream()：
// 帧的形状（assistant.delta → 最后结算）在 S2 就定死了，这里只是把"整段"拆成"增量"。
#include <nlohmann/json.hpp>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace core {

using json = nlohmann::json;

struct Message {
  std::string role;  // "system" | "user" | "assistant"
  std::string content;
};

struct ChatResult {
  std::string text;
  std::string finish_reason;  // "stop" | "length" | "content_filter" ...
  json usage = json::object();
};

// Provider 的错误：引擎捕获后统一变成 PROVIDER_ERROR 响应（见 docs/stage-s3a.md）
struct ProviderError : std::runtime_error {
  explicit ProviderError(const std::string& what) : std::runtime_error(what) {}
};

// 增量回调：每收到一小段新文本就调一次（可能是几个字，也可能是一个字）
using DeltaSink = std::function<void(const std::string& delta)>;

class Provider {
 public:
  virtual ~Provider() = default;

  // 同步调用：成功返回整段回复；失败抛 ProviderError（或标准异常）
  virtual ChatResult chat(const std::vector<Message>& messages) = 0;

  // 流式调用：边收边调 on_delta；返回值仍是"本轮结算"（形状与 chat() 相同）。
  // 默认实现 = 退化成非流式（一次性把整段当做一个增量吐出来）：
  // 新增一种 provider 时不必强制实现流式——引擎那边只有一行 if 的差别。
  virtual ChatResult chat_stream(const std::vector<Message>& messages, const DeltaSink& on_delta) {
    ChatResult r = chat(messages);
    if (!r.text.empty() && on_delta) on_delta(r.text);
    return r;
  }

  // 引擎据此决定走 chat_stream 还是 chat（默认实现是"没有真增量"）
  virtual bool supports_stream() const { return false; }
};

}  // namespace core
