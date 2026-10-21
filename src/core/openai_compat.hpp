#pragma once
// OpenAI 兼容端点（DeepSeek / OpenAI / 本地 vLLM 都是这一族）。
// S3a：非流式；S3b：`stream: true` + SSE 增量，非流式作为回退保留；
// S4：请求带 `tools`，回复带 `tool_calls`（流式下 arguments 是分片累积出来的）。
#include "core/provider.hpp"

#include <string>

namespace core {

struct OpenAIConfig {
  std::string base_url;  // https://api.deepseek.com 或 http://127.0.0.1:18080
  std::string api_key;   // 只从环境变量/参数来，绝不进仓库
  std::string model;     // 例：deepseek-chat
  int timeout_ms = 120000;

  // —— S3b ——
  bool stream = true;    // 默认流式；false 回到 S3a 的整段返回（--no-stream）
  // 只在**传输层失败**且"这一轮还一个字都没吐出去"时重试：
  // 吐过字再重试会让前端看到重复内容；状态码错误（4xx/5xx）不重试，如实上报。
  int max_attempts = 2;
};

// OpenAI 兼容客户端：
//   chat()         POST {base}/chat/completions，stream=false
//   chat_stream()  POST {base}/chat/completions，stream=true，逐段回调
// 对端无视 stream:true、直接回整段 JSON 时，chat_stream() 自动回退到非流式解析。
class OpenAICompatProvider : public Provider {
 public:
  explicit OpenAICompatProvider(OpenAIConfig cfg);

  ChatResult chat(const ChatRequest& req) override;
  ChatResult chat_stream(const ChatRequest& req, const DeltaSink& on_delta) override;
  bool supports_stream() const override { return true; }

 private:
  void require_credentials() const;

  // 一次流式尝试。成功即返回本轮结算；"这次没成、可以安全重试"的错误抛 RetryableError。
  // emitted 是跨尝试的累计状态：已经吐过字就不许重试（会重复输出）。
  ChatResult stream_once(const std::string& origin, const std::string& path, const std::string& body,
                         const DeltaSink& on_delta, bool& emitted);

  OpenAIConfig cfg_;
};

}  // namespace core
