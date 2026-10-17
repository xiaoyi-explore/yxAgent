#include "core/openai_compat.hpp"

#include <httplib.h>

#include <string>
#include <utility>

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

}  // namespace

OpenAICompatProvider::OpenAICompatProvider(OpenAIConfig cfg) : cfg_(std::move(cfg)) {}

ChatResult OpenAICompatProvider::chat(const std::vector<Message>& messages) {
  if (cfg_.model.empty()) throw ProviderError("no model configured (pass --model, e.g. deepseek-chat)");
  if (cfg_.api_key.empty())
    throw ProviderError("no api key (set DEEPSEEK_API_KEY in the environment, or pass --api-key)");

  const Endpoint ep = parse_endpoint(cfg_.base_url);
  httplib::Client cli(ep.origin);
  // 连接与读取分开设：连不上要快失败，读（模型思考）可以慢
  cli.set_connection_timeout(10, 0);
  cli.set_read_timeout(cfg_.timeout_ms / 1000, (cfg_.timeout_ms % 1000) * 1000);
  cli.set_follow_location(true);

  json body = {{"model", cfg_.model}, {"stream", false}, {"messages", json::array()}};
  for (const auto& m : messages) body["messages"].push_back({{"role", m.role}, {"content", m.content}});

  const httplib::Headers headers = {{"Authorization", "Bearer " + cfg_.api_key}};
  const auto res = cli.Post(ep.path, headers, body.dump(), "application/json");
  if (!res) {
    throw ProviderError("HTTP transport error: " + httplib::to_string(res.error()) + " (" + ep.origin + ep.path + ")");
  }
  if (res->status != 200) {
    throw ProviderError("HTTP " + std::to_string(res->status) + " from " + ep.origin + ep.path + ": " +
                        snippet(res->body));
  }

  const json j = json::parse(res->body, nullptr, false);
  if (j.is_discarded()) throw ProviderError("response is not JSON: " + snippet(res->body));
  if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
    throw ProviderError("response has no choices: " + snippet(res->body));

  const json& c0 = j["choices"][0];
  ChatResult r;
  if (c0.contains("message") && c0["message"].contains("content") && c0["message"]["content"].is_string())
    r.text = c0["message"]["content"].get<std::string>();
  r.finish_reason = c0.value("finish_reason", "");
  if (j.contains("usage") && j["usage"].is_object()) r.usage = j["usage"];
  return r;
}

}  // namespace core
