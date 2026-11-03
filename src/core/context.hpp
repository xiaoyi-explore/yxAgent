#pragma once
// ContextManager（S6）—— 让上下文不爆。
//
// 三件事，按"便宜到贵"的顺序做：
//   1. **工具结果裁剪**：老的非检索类工具结果截断到 2KB（不丢消息，只砍体积）；
//   2. **摘要压缩**：估算超过预算的一半时，把最老的若干**整轮**折叠成一条抽取式摘要；
//   3. **兜底**：压完还超预算，就连检索类结果一起截断（"永不压缩"是默认，不是绝对）。
//
// 为什么压缩不只是省钱：**context rot**（Anthropic）——上下文越长，模型对早期事实的召回越差。
// 所以压缩是"保效果"，省 token 只是副产品。
//
// 本文件不认识 HTTP、不认识工具注册表、也不认识协议：它只操作 vector<Message>。
#include "core/provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace core {

struct ContextConfig {
  // 预算（估算 token）。0 = 直接关掉整个 S6（给"我要看原始行为"用）。
  size_t max_tokens = 32000;
  // 触发线：估算超过 max_tokens * threshold 才开始摘要压缩。50% 是 Gemini CLI 的实测值——
  // 等到 90% 才压，你会先被"一次多轮工具调用"顶爆。
  double threshold = 0.5;
  // 最近几轮原样保留（轮 = 一个 user 消息 + 它引发的所有 assistant/tool 消息）。
  int keep_recent_turns = 3;
  // 老的非检索类工具结果截断到这个字节数。
  size_t tool_result_cap = 2048;
};

struct CompactReport {
  bool did = false;              // 这次到底改没改历史
  size_t before_tokens = 0;      // 估算值（不是 tokenizer）
  size_t after_tokens = 0;
  size_t dropped_messages = 0;   // 被折叠掉的消息条数
  size_t summarized_turns = 0;   // 被折叠的轮数
  size_t truncated_tools = 0;    // 被截断的工具结果条数
};

// token 估算：**字节 / 4 + 每条消息的固定开销**。
//   * 英文 ≈ 4 字节/token，这条几乎准确；
//   * 中文一个字 3 字节 ≈ 0.75 token，会略微低估——所以预算要留余量。
// 它是**估算**，不是 tokenizer。真正的账本是响应里的 usage（S3b 起流式也带）。
size_t estimate_tokens(const Message& m);
size_t estimate_tokens(const std::vector<Message>& msgs);

class ContextManager {
 public:
  explicit ContextManager(ContextConfig cfg) : cfg_(std::move(cfg)) {}

  // 就地压缩。没到触发线且没有可截断的工具结果时，history 一个字不动、report.did == false。
  CompactReport compact(std::vector<Message>& history) const;

  const ContextConfig& config() const { return cfg_; }

 private:
  ContextConfig cfg_;
};

}  // namespace core
