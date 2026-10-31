#include "core/openai_compat.hpp"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <utility>

#include "core/sse.hpp"

namespace core {
namespace {

// 把 base_url 拆成 (scheme+host+port, path)：httplib::Client 吃前者，请求吃后者。
//    https://api.deepseek.com        -> origin=https://api.deepseek.com, path=/chat/completions
//    https://api.deepseek.com/v1     -> origin=https://api.deepseek.com, path=/v1/chat/completions
//    http://127.0.0.1:18080          -> origin=http://127.0.0.1:18080,     path=/chat/completions
struct Endpoint {
  std::string origin;
  std::string path;
};

Endpoint parse_endpoint(const std::string& base_url) {
  std::string url = base_url.empty() ? "https://api.deepseek.com" : base_url;
  if (url.find("://") == std::string::npos) url = "http://" + url;  // 省略 scheme 按 http（本地调试常见）
  const auto scheme_end = url.find("://") + 3;
  const auto slash = url.find('/', scheme_end);
  Endpoint ep;
  if (slash == std::string::npos) {
    ep.origin = url;
    ep.path = "/chat/completions";
  } else {
    ep.origin = url.substr(0, slash);
    std::string prefix = url.substr(slash);
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    ep.path = prefix + "/chat/completions";
  }
  return ep;
}

std::string snippet(const std::string& s, size_t n = 300) {
  if (s.size() <= n) return s;
  return s.substr(0, n) + "...";
}

// 可重试 = **传输层**失败（连不上 / 连接被掐断 / 读超时），也就是"这一个请求根本没落地"。
// 为什么 5xx 不重试：S3a 已经把契约写成"一次请求 = 一次尝试"（tests/test_s3a.py 的 failonce
// 用例靠它测"失败的一轮不回灌历史"）。状态码级的重试策略属于 S5 的取消/重试，不在这里偷偷做。
struct RetryableError : ProviderError {
  explicit RetryableError(const std::string& what) : ProviderError(what) {}
};

// ---- wire format：core 的 Message -> OpenAI 的 JSON ----
// 只在这里出现 OpenAI 的字段名，别的地方都只认 core::Message（换厂商时只改这里）。
json to_wire(const Message& m) {
  json j = {{"role", m.role}, {"content", m.content}};
  if (!m.tool_calls.empty()) {
    j["tool_calls"] = json::array();
    for (const ToolCall& c : m.tool_calls) {
      j["tool_calls"].push_back({{"id", c.id},
                                 {"type", "function"},
                                 {"function", {{"name", c.name}, {"arguments", c.arguments}}}});
    }
  }
  if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
  if (!m.name.empty() && m.role == "tool") j["name"] = m.name;
  return j;
}

json make_body(const OpenAIConfig& cfg, const ChatRequest& req, bool stream) {
  json body = {{"model", cfg.model}, {"stream", stream}, {"messages", json::array()}};
  for (const Message& m : req.messages) body["messages"].push_back(to_wire(m));

  if (!req.tools.empty()) {
    body["tools"] = json::array();
    for (const ToolSpec& t : req.tools) {
      body["tools"].push_back({{"type", "function"},
                               {"function", {{"name", t.name},
                                             {"description", t.description},
                                             {"parameters", t.parameters}}}});
    }
  }
  // OpenAI 的流式默认**不带** usage，要显式开口（docs/protocol.md §9 S3b）
  if (stream) body["stream_options"] = {{"include_usage", true}};
  return body;
}

// 一段完整的 assistant 消息 -> ChatResult 的公共部分（非流式路径用）
void parse_assistant_message(const json& m, ChatResult& out) {
  if (m.contains("content") && m["content"].is_string()) out.text = m["content"].get<std::string>();
  if (!m.contains("tool_calls") || !m["tool_calls"].is_array()) return;
  for (const json& tc : m["tool_calls"]) {
    if (!tc.is_object()) continue;
    ToolCall call;
    call.id = tc.value("id", "");
    if (tc.contains("function") && tc["function"].is_object()) {
      call.name = tc["function"].value("name", "");
      call.arguments = tc["function"].value("arguments", "");
    }
    out.tool_calls.push_back(std::move(call));
  }
}

// 整段 JSON 回复 -> ChatResult。S3a 的路径；S3b 的"非流式回退"也走这里。
ChatResult parse_completion_body(const std::string& body) {
  const json j = json::parse(body, nullptr, false);
  if (j.is_discarded()) throw ProviderError("response is not JSON: " + snippet(body));
  if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
    throw ProviderError("response has no choices: " + snippet(body));

  const json& c0 = j["choices"][0];
  ChatResult r;
  if (c0.contains("message") && c0["message"].is_object()) parse_assistant_message(c0["message"], r);
  r.finish_reason = c0.value("finish_reason", "");
  if (j.contains("usage") && j["usage"].is_object()) r.usage = j["usage"];
  return r;
}

// 连接与读取分开设：连不上要快失败，读（模型思考 / 慢速流）可以慢
void setup_client(httplib::Client& cli, const OpenAIConfig& cfg) {
  cli.set_connection_timeout(10, 0);
  cli.set_read_timeout(cfg.timeout_ms / 1000, (cfg.timeout_ms % 1000) * 1000);
  cli.set_follow_location(true);
}

// 非流式回退时最多攒这么多字节（足够容纳一整段回复；SSE 路径下 raw 在第一帧后就不再增长）
constexpr size_t kFallbackCap = 1u << 20;

}  // namespace

OpenAICompatProvider::OpenAICompatProvider(OpenAIConfig cfg) : cfg_(std::move(cfg)) {}

void OpenAICompatProvider::require_credentials() const {
  if (cfg_.model.empty()) throw ProviderError("no model configured (pass --model, e.g. deepseek-chat)");
  if (cfg_.api_key.empty())
    throw ProviderError("no api key (set DEEPSEEK_API_KEY in the environment, or pass --api-key)");
}

ChatResult OpenAICompatProvider::chat(const ChatRequest& req) {
  require_credentials();
  const Endpoint ep = parse_endpoint(cfg_.base_url);

  httplib::Client cli(ep.origin);
  setup_client(cli, cfg_);

  const httplib::Headers headers = {{"Authorization", "Bearer " + cfg_.api_key}};
  const auto res = cli.Post(ep.path, headers, make_body(cfg_, req, false).dump(), "application/json");
  if (!res) {
    throw ProviderError("HTTP transport error: " + httplib::to_string(res.error()) + " (" + ep.origin + ep.path + ")");
  }
  if (res->status != 200) {
    throw ProviderError("HTTP " + std::to_string(res->status) + " from " + ep.origin + ep.path + ": " +
                        snippet(res->body));
  }
  return parse_completion_body(res->body);
}

ChatResult OpenAICompatProvider::chat_stream(const ChatRequest& req, const DeltaSink& on_delta) {
  require_credentials();
  const Endpoint ep = parse_endpoint(cfg_.base_url);
  const std::string body = make_body(cfg_, req, true).dump();
  const int attempts = std::max(1, cfg_.max_attempts);

  bool emitted = false;  // 已经交给前端增量了吗？——决定重试安不安全
  for (int attempt = 1;; ++attempt) {
    try {
      return stream_once(ep.origin, ep.path, body, on_delta, emitted);
    } catch (const RetryableError& e) {
      // 吐过字就不重试：用户会看到重复的半句话，比报错更糟
      if (emitted || attempt >= attempts) throw ProviderError(e.what());
      std::this_thread::sleep_for(std::chrono::milliseconds(120 * attempt));  // 简单退避
    }
  }
}

ChatResult OpenAICompatProvider::stream_once(const std::string& origin, const std::string& path,
                                             const std::string& body, const DeltaSink& on_delta,
                                             bool& emitted) {
  httplib::Client cli(origin);
  setup_client(cli, cfg_);

  ChatResult out;
  std::string raw;           // 开头若干字节：错误正文、非流式回退都要看它
  std::string stream_error;  // 网关在流里插的 {"error":...}
  bool saw_json = false;     // 收到过合法的 SSE JSON 事件 = 对端确实在按 SSE 说话
  bool aborted = false;      // S5：引擎喊停了（取消）。它必须变成 content_receiver 的 false

  // S4 的头号坑：tool_calls 的 arguments 是**分片**到达的，必须按 index 累积拼接。
  // 用 map 而不是 vector：分片可能乱序（先 index=1 再 index=0），键序负责还原顺序。
  std::map<int, ToolCall> partial_calls;

  SseParser parser([&](const SseEvent& ev) {
    if (aborted) return;  // 喊停之后还在缓冲里的帧：一帧都不再处理
    if (ev.data.empty()) return;
    if (ev.data == "[DONE]") return;  // 流结束标记：后面不再有 JSON

    const json j = json::parse(ev.data, nullptr, false);
    if (j.is_discarded()) return;  // 不是 JSON 的 data 行：忽略（容忍网关插播的杂音）
    saw_json = true;

    if (j.contains("error") && j["error"].is_object() && stream_error.empty())
      stream_error = "model returned an error event: " + snippet(j["error"].dump());
    if (j.contains("usage") && j["usage"].is_object()) out.usage = j["usage"];  // include_usage 的那一帧
    if (!j.contains("choices") || !j["choices"].is_array()) return;

    for (const json& c : j["choices"]) {
      if (!c.is_object()) continue;
      if (c.contains("delta") && c["delta"].is_object()) {
        const json& d = c["delta"];
        if (d.contains("content") && d["content"].is_string()) {
          const std::string piece = d["content"].get<std::string>();
          if (!piece.empty()) {
            out.text += piece;  // 结算 = 增量的顺序拼接（protocol.md §9 不变量 2）
            emitted = true;
            // S5：false = 引擎要停（取消）。记下来，让 content_receiver 把这次传输掐掉。
            if (on_delta && !on_delta(piece)) aborted = true;
          }
        }
        if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
          for (const json& tc : d["tool_calls"]) {
            if (!tc.is_object()) continue;
            ToolCall& slot = partial_calls[tc.value("index", 0)];  // 按 index 累积，不是按数组下标
            if (tc.contains("id") && tc["id"].is_string()) {
              const std::string id = tc["id"].get<std::string>();
              if (!id.empty()) slot.id = id;  // id 只来一次
            }
            if (tc.contains("function") && tc["function"].is_object()) {
              const json& f = tc["function"];
              if (f.contains("name") && f["name"].is_string())
                slot.name += f["name"].get<std::string>();  // 名字也可能被切开
              if (f.contains("arguments") && f["arguments"].is_string())
                slot.arguments += f["arguments"].get<std::string>();  // 参数永远是分片
            }
          }
        }
      }
      if (c.contains("finish_reason") && c["finish_reason"].is_string())
        out.finish_reason = c["finish_reason"].get<std::string>();
    }
  });

  // 坑（S3b 踩过）：cpp-httplib 的 **Client::Post 没有 ContentReceiver 重载**——
  // 只有 Get 有。想"边收边解析"必须自己拼一个 Request，把回调塞进 req.content_receiver，
  // 再走 send(req, res, err)。写成 cli.Post(..., receiver) 会变成编译错误。
  httplib::Request req;
  req.method = "POST";
  req.path = path;
  req.body = body;
  req.set_header("Authorization", "Bearer " + cfg_.api_key);
  req.set_header("Accept", "text/event-stream");
  req.set_header("Content-Type", "application/json");
  req.content_receiver = [&](const char* data, size_t len, uint64_t /*offset*/, uint64_t /*total*/) {
    parser.feed(data, len);
    if (!saw_json && raw.size() < kFallbackCap)
      raw.append(data, std::min(len, kFallbackCap - raw.size()));
    // 返回值就是"还要不要继续收"：false 会让 httplib 中止这次传输（取消穿透到 HTTP 的那一步）。
    return !aborted;
  };

  httplib::Response res;
  httplib::Error err = httplib::Error::Success;
  const bool sent = cli.send(req, res, err);
  parser.flush();  // 对端可能断在一个事件中间：把已经完整的部分结算掉

  // 取消：这是**我们自己**掐的，既不是传输失败（别重试），也不是对端报错（别报 PROVIDER_ERROR）。
  // 必须放在 !sent 之前判断——主动断流时 httplib 一定会返回 false + Error::Canceled。
  if (aborted) {
    out.cancelled = true;
    if (out.finish_reason.empty()) out.finish_reason = "cancelled";
    return out;
  }

  if (!sent) {
    throw RetryableError("HTTP transport error: " + httplib::to_string(err) + " (" + origin + path + ")");
  }
  if (res.status != 200) {
    const std::string detail = raw.empty() ? res.body : raw;
    throw ProviderError("HTTP " + std::to_string(res.status) + " from " + origin + path + ": " +
                        snippet(detail));
  }
  if (!stream_error.empty()) throw ProviderError(stream_error);

  if (!saw_json) {
    // 对端无视了 stream:true，直接回一整段 JSON —— 回退到 S3a 的解析（没有增量，结果一样）
    ChatResult r = parse_completion_body(raw.empty() ? res.body : raw);
    if (!r.text.empty() && on_delta) on_delta(r.text);
    if (!r.text.empty()) emitted = true;
    return r;
  }

  for (auto& kv : partial_calls) out.tool_calls.push_back(std::move(kv.second));  // 键序 = index 序
  return out;
}

}  // namespace core
