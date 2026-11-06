#include "core/context.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <utility>

namespace core {
namespace {

// 检索类工具：结果是要"留着查"的事实，压缩它就等于压缩证据。
// （stage-s6.md 的约定：新加工具时要在这里表个态——它算不算检索类。）
bool is_retrieval(const std::string& tool_name) { return tool_name == "read_file"; }

constexpr const char* kMarker = "[压缩]";
constexpr size_t kSummaryCap = 1600;   // 摘要最多这么长（字节），否则它自己又变成一个大包袱
constexpr size_t kUserLineCap = 100;   // 摘要里每条"用户说"的截断长度
constexpr size_t kAssistantLineCap = 40;
constexpr size_t kToolFloor = 256;     // 兜底时工具结果最小留多少（再小就没信息了）

// 压成一行：摘要里不能有换行，否则它看起来像好几条消息
std::string one_line(const std::string& s, size_t cap) {
  const size_t n = proto::utf8_prefix_bytes(s, cap);
  std::string out = s.substr(0, n);
  for (char& c : out)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  if (n < s.size()) out += "…";
  return out;
}

bool is_summary(const Message& m) {
  return m.role == "user" && m.content.rfind(kMarker, 0) == 0;
}

// 上一次压缩留下的摘要正文（去掉头部那一行），用来"接着往后写"而不是套娃。
std::string summary_body(const std::string& content) {
  const size_t nl = content.find('\n');
  return nl == std::string::npos ? std::string() : content.substr(nl + 1);
}

std::string cut_marker(size_t orig, size_t kept) {
  return "\n[已截断：原始 " + std::to_string(orig) + " 字节，只保留前 " + std::to_string(kept) + "]";
}

}  // namespace

size_t estimate_tokens(const Message& m) {
  // role + 内容 + JSON 结构开销（键名、引号、逗号）。8 是经验值。
  size_t bytes = m.role.size() + m.content.size() + 8;
  for (const ToolCall& c : m.tool_calls)
    bytes += c.id.size() + c.name.size() + c.arguments.size() + 16;
  if (!m.tool_call_id.empty()) bytes += m.tool_call_id.size() + 16;
  if (!m.name.empty()) bytes += m.name.size() + 8;
  return bytes / 4 + 1;
}

size_t estimate_tokens(const std::vector<Message>& msgs) {
  size_t total = 0;
  for (const Message& m : msgs) total += estimate_tokens(m);
  return total;
}

CompactReport ContextManager::compact(std::vector<Message>& history) const {
  CompactReport rep;
  rep.before_tokens = estimate_tokens(history);
  if (cfg_.max_tokens == 0 || history.empty()) {
    rep.after_tokens = rep.before_tokens;
    return rep;
  }

  // ── 第一级：工具结果裁剪（最便宜）──────────────────────────────────────
  // 不丢消息，只砍体积；**最近一轮不动**（模型正在用它推理）。
  size_t last_user = 0;
  for (size_t i = history.size(); i-- > 0;)
    if (history[i].role == "user" && !is_summary(history[i])) { last_user = i; break; }

  for (size_t i = 0; i < history.size(); ++i) {
    if (history[i].role != "tool") continue;
    if (i > last_user) continue;                    // 本轮正在用的结果：别动
    if (is_retrieval(history[i].name)) continue;    // 检索类：永不主动压缩
    if (history[i].content.size() <= cfg_.tool_result_cap) continue;
    const size_t orig = history[i].content.size();
    const size_t kept = proto::utf8_prefix_bytes(history[i].content, cfg_.tool_result_cap);
    if (kept >= orig) continue;
    history[i].content.resize(kept);
    history[i].content += cut_marker(orig, kept);
    ++rep.truncated_tools;
  }

  // ── 第三级：兜底（压完还爆就继续压，直到进预算或到地板）─────────────────
  // "检索类永不压缩"是默认，不是绝对：请求被对端拒绝比丢一点证据更糟。
  const auto hard_truncate = [&]() {
    size_t cap = cfg_.tool_result_cap;
    while (estimate_tokens(history) > cfg_.max_tokens && cap >= kToolFloor) {
      for (Message& m : history) {
        if (m.role != "tool") continue;
        if (m.content.size() <= cap) continue;
        const size_t orig = m.content.size();
        const size_t kept = proto::utf8_prefix_bytes(m.content, cap);
        if (kept >= orig) continue;
        m.content.resize(kept);
        m.content += cut_marker(orig, kept);
        ++rep.truncated_tools;
      }
      cap /= 2;
    }
  };

  const auto finish = [&]() {
    hard_truncate();
    rep.after_tokens = estimate_tokens(history);
    if (rep.truncated_tools > 0 || rep.summarized_turns > 0) rep.did = true;
    return rep;
  };

  // ── 第二级：超阈值 → 摘要压缩 ───────────────────────────────────────────
  const size_t trigger = static_cast<size_t>(static_cast<double>(cfg_.max_tokens) * cfg_.threshold);
  if (estimate_tokens(history) <= trigger) return finish();

  const bool has_system = history[0].role == "system";
  const size_t begin = has_system ? 1 : 0;

  // 切点 = 倒数第 keep_recent_turns 个 user（摘要不算"一轮"）。轮数不够就不压。
  size_t cut = history.size();
  int users = 0;
  for (size_t i = history.size(); i-- > 0;) {
    if (history[i].role != "user") continue;
    if (is_summary(history[i])) continue;
    if (++users == std::max(1, cfg_.keep_recent_turns)) { cut = i; break; }
  }
  if (cut <= begin) return finish();

  // [begin, cut) 按"轮"分组。轮 = 一个 user 到下一个 user 之前。
  // 含检索类工具结果的轮**整轮豁免**：检索结果不能丢，而只留 tool 消息会破坏配对
  // （tool 消息前面必须有 assistant(tool_calls)，否则请求直接被 API 拒绝）。
  std::vector<Message> keep;
  std::string carry;   // 上一次压缩的摘要正文：接着写，不许套娃（套娃会让 token 反而变大）
  std::string summary;
  size_t dropped = 0, turns = 0;

  size_t i = begin;
  while (i < cut) {
    size_t j = i + 1;
    while (j < cut && history[j].role != "user") ++j;  // 一轮 = [i, j)

    if (is_summary(history[i])) {          // 旧摘要：只搬运，不再折叠一次
      carry = summary_body(history[i].content);
      i = j;
      continue;
    }

    bool has_retrieval = false;
    for (size_t k = i; k < j; ++k)
      if (history[k].role == "tool" && is_retrieval(history[k].name)) has_retrieval = true;

    if (has_retrieval) {
      for (size_t k = i; k < j; ++k) keep.push_back(history[k]);
    } else {
      std::string line = "- 用户：" + one_line(history[i].content, kUserLineCap);
      for (size_t k = i + 1; k < j; ++k) {
        if (history[k].role == "assistant" && !history[k].content.empty()) {
          line += " → 助手：" + one_line(history[k].content, kAssistantLineCap);
          break;
        }
      }
      if (carry.size() + summary.size() + line.size() + 2 <= kSummaryCap) summary += line + "\n";
      dropped += (j - i);
      ++turns;
    }
    i = j;
  }

  if (turns == 0) return finish();   // 老轮全是"检索类"或者只有一个旧摘要：这一步无事可做

  std::string body = carry + summary;   // 旧摘要（更早）在前
  if (body.size() > kSummaryCap) body.resize(proto::utf8_prefix_bytes(body, kSummaryCap));

  std::vector<Message> rebuilt;
  rebuilt.reserve(history.size());
  for (size_t k = 0; k < begin; ++k) rebuilt.push_back(history[k]);
  rebuilt.push_back(Message{"user", std::string(kMarker) + " 更早的 " + std::to_string(turns) +
                                        " 轮已折叠（原文省略）：\n" + body});
  for (const Message& m : keep) rebuilt.push_back(m);
  for (size_t k = cut; k < history.size(); ++k) rebuilt.push_back(history[k]);

  // 硬保证：压缩不许把历史撑大（短对话里摘要的固定开销可能比原文还大）。
  if (estimate_tokens(rebuilt) >= estimate_tokens(history)) return finish();

  history = std::move(rebuilt);
  rep.dropped_messages = dropped;
  rep.summarized_turns = turns;
  return finish();
}

}  // namespace core