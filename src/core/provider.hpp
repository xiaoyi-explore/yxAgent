#pragma once
// Provider —— core 与"某个模型服务"之间的接缝。
// S3a 只做非流式（一次拿到整段回复）；S3b 会在同一接缝上长出 stream()。
#include <nlohmann/json.hpp>
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

class Provider {
 public:
  virtual ~Provider() = default;
  // 同步调用：成功返回整段回复；失败抛 ProviderError（或标准异常）
  virtual ChatResult chat(const std::vector<Message>& messages) = 0;
};

}  // namespace core
