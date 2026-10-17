#pragma once
// OpenAI 兼容端点（DeepSeek / OpenAI / 本地 vLLM 都是这一族）—— S3a：非流式。
#include "core/provider.hpp"

#include <string>

namespace core {

struct OpenAIConfig {
  std::string base_url;  // https://api.deepseek.com 或 http://127.0.0.1:18080
  std::string api_key;   // 只从环境变量/参数来，绝不进仓库
  std::string model;     // 例：deepseek-chat
  int timeout_ms = 120000;
};

// POST {base_url}/chat/completions，stream=false。
class OpenAICompatProvider : public Provider {
 public:
  explicit OpenAICompatProvider(OpenAIConfig cfg);
  ChatResult chat(const std::vector<Message>& messages) override;

 private:
  OpenAIConfig cfg_;
};

}  // namespace core
