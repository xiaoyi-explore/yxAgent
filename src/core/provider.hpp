#pragma once
// Provider —— core 与"某个模型服务"之间的接缝。
// S3a 只有非流式；S3b 长出 chat_stream()（把"整段"拆成"增量"）；
// S4 再长出"工具"：请求里带 ToolSpec，回复里带 ToolCall。
//
// 这一层只管**格式**（OpenAI 的 wire format），不管**语义**：
// 工具要不要执行、执行几次、循环几轮，全在 core 的 agent 循环里（docs/stage-s4.md）。
#include <nlohmann/json.hpp>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace core {

using json = nlohmann::json;

// 模型要求的一次工具调用。arguments 是模型给的 JSON **文本**（不是对象）：
// 流式下它是分片到达、拼接起来的，解析与校验由 agent 循环负责——这里一个字都不猜。
struct ToolCall {
  std::string id;         // 模型给的调用 id：tool 消息必须带着它回去
  std::string name;       // 工具名
  std::string arguments;  // JSON 文本（可能是坏 JSON：调用方必须能处理）
};

struct Message {
  std::string role;  // "system" | "user" | "assistant" | "tool"
  std::string content;

  // —— 以下两个字段只在特定 role 上有意义 ——
  std::vector<ToolCall> tool_calls;  // assistant：这一轮它要求调用哪些工具
  std::string tool_call_id;          // tool：这条结果回应的是哪个调用
  std::string name;                  // tool：函数名（给人看的，不参与协议判定）

  Message() = default;
  Message(std::string r, std::string c) : role(std::move(r)), content(std::move(c)) {}
};

// 声明一个工具（发给模型的 JSON Schema）
struct ToolSpec {
  std::string name;
  std::string description;
  json parameters = json::object();  // JSON Schema（type/properties/required）
};

struct ChatRequest {
  std::vector<Message> messages;
  std::vector<ToolSpec> tools;  // 空 = 这一轮不允许用工具（S3a/S3b 的行为）
};

struct ChatResult {
  std::string text;
  std::string finish_reason;  // "stop" | "length" | "tool_calls" | "content_filter" ...
  json usage = json::object();
  std::vector<ToolCall> tool_calls;  // 非空 = 模型要求调工具（此时 text 通常是空的）
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
  virtual ChatResult chat(const ChatRequest& req) = 0;

  // 流式调用：边收边调 on_delta；返回值仍是"本轮结算"（形状与 chat() 相同）。
  // 默认实现 = 退化成非流式（一次性把整段当做一个增量吐出来）：
  // 新增一种 provider 时不必强制实现流式——引擎那边只有一行 if 的差别。
  virtual ChatResult chat_stream(const ChatRequest& req, const DeltaSink& on_delta) {
    ChatResult r = chat(req);
    if (!r.text.empty() && on_delta) on_delta(r.text);
    return r;
  }

  // 引擎据此决定走 chat_stream 还是 chat（默认实现是"没有真增量"）
  virtual bool supports_stream() const { return false; }
};

}  // namespace core
