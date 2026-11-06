// S6 的纯单元测试：只编译 context.cpp —— 不链接 httplib / 工具层 / 引擎。
// 等于顺手用构建系统证明"ContextManager 不认识网络、不认识协议"（和 sse_parser_test 同一个套路）。
//
// 为什么压缩值得单独测：它的正确性全是**结构性**的（配对、边界、豁免、不许变大），
// 端到端跑一遍只能看出"没崩"，看不出"少了一条 tool 消息"这种要命的细节。
#include "core/context.hpp"

#include <cstdio>
#include <string>
#include <vector>

using core::CompactReport;
using core::ContextConfig;
using core::ContextManager;
using core::Message;
using core::ToolCall;
using core::estimate_tokens;

namespace {

int g_fail = 0;

void check(const std::string& name, bool ok, const std::string& detail = "") {
  std::printf("%s %s%s\n", ok ? "PASS" : "FAIL", name.c_str(),
              (!ok && !detail.empty()) ? ("   " + detail).c_str() : "");
  if (!ok) ++g_fail;
}

Message u(const std::string& s) { return Message{"user", s}; }
Message a(const std::string& s) { return Message{"assistant", s}; }
Message sys(const std::string& s) { return Message{"system", s}; }

Message tool_msg(const std::string& name, const std::string& content) {
  Message m{"tool", content};
  m.name = name;
  m.tool_call_id = "call_" + name;
  return m;
}

Message assistant_calls(const std::string& name) {
  Message m{"assistant", ""};
  ToolCall c;
  c.id = "call_" + name;
  c.name = name;
  c.arguments = "{}";
  m.tool_calls.push_back(c);
  return m;
}

bool has_orphan_tool(const std::vector<Message>& h) {
  bool armed = false;
  for (const Message& m : h) {
    if (m.role == "assistant") armed = !m.tool_calls.empty();
    if (m.role == "tool" && !armed) return true;
  }
  return false;
}

std::string join_all(const std::vector<Message>& h) {
  std::string s;
  for (const Message& m : h) s += m.content + "\n";
  return s;
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

size_t count_role(const std::vector<Message>& h, const std::string& role) {
  size_t n = 0;
  for (const Message& m : h) if (m.role == role) ++n;
  return n;
}

size_t index_of_role(const std::vector<Message>& h, const std::string& role) {
  for (size_t i = 0; i < h.size(); ++i) if (h[i].role == role) return i;
  return h.size();
}

// 一根长一点的"用户提问"：真实对话里的消息不会只有五个字，压缩在短消息上是赚不回来的。
std::string big_question(const std::string& tag) { return tag + std::string(180, 'x'); }

}  // namespace

int main() {
  // ---- 1. 估算：单调、且大致是"字节/4" ----
  {
    std::vector<Message> h{sys("你是助手"), u("hi"), a("hello")};
    const size_t t1 = estimate_tokens(h);
    h.push_back(u(std::string(400, 'x')));
    const size_t t2 = estimate_tokens(h);
    check("估算：多一条长消息，估算变大", t2 > t1, std::to_string(t1) + " -> " + std::to_string(t2));
    check("估算：400 字节 ≈ 100 token", t2 - t1 >= 100 && t2 - t1 <= 120, std::to_string(t2 - t1));
  }

  // ---- 2. 没到阈值：一个字都不动 ----
  {
    std::vector<Message> h{sys("s"), u("1"), a("1"), u("2"), a("2")};
    const size_t n = h.size();
    ContextManager cm(ContextConfig{/*max*/ 100000, 0.5, 1, 2048});
    const CompactReport r = cm.compact(h);
    check("未超阈值：不动历史", !r.did && h.size() == n && r.dropped_messages == 0);
  }

  // ---- 3. max_tokens = 0：整个 S6 关掉 ----
  {
    std::vector<Message> h{sys("s")};
    for (int i = 0; i < 50; ++i) { h.push_back(u(big_question("q"))); h.push_back(a("ok")); }
    ContextManager cm(ContextConfig{/*max*/ 0, 0.5, 1, 2048});
    const CompactReport r = cm.compact(h);
    check("预算=0：完全不压缩（关掉 S6）", !r.did && h.size() == 101, std::to_string(h.size()));
  }

  // ---- 4. 折叠老轮 + 保留最近轮 + system 在第 0 条 + 早期事实进摘要 ----
  {
    std::vector<Message> h{sys("系统提示：代号蓝鲸")};
    h.push_back(u("早期事实：密码是42"));          // 第一轮，最终会被折叠
    h.push_back(a("记住了"));
    for (int i = 1; i <= 5; ++i) {
      h.push_back(u(big_question("第" + std::to_string(i) + "个问题")));
      h.push_back(a("第" + std::to_string(i) + "个回答"));
    }
    ContextManager cm(ContextConfig{/*max*/ 120, 0.5, /*keep*/ 2, 2048});
    const CompactReport r = cm.compact(h);
    check("折叠：确实压了", r.did && r.summarized_turns >= 1, std::to_string(r.summarized_turns));
    check("折叠：token 下降", r.after_tokens < r.before_tokens,
          std::to_string(r.before_tokens) + " -> " + std::to_string(r.after_tokens));
    check("折叠：system 仍在第 0 条且原样", h[0].role == "system" && contains(h[0].content, "蓝鲸"));
    check("折叠：第 1 条变成摘要消息", h.size() > 1 && contains(h[1].content, "[压缩]"),
          h.size() > 1 ? h[1].content.substr(0, 40) : "");
    check("折叠：摘要里留下了早期事实（抽取式）", contains(join_all(h), "密码是42"),
          join_all(h).substr(0, 140));
    check("折叠：最近 2 轮原样保留", contains(join_all(h), "第5个回答") && contains(join_all(h), "第4个回答"));
  }

  // ---- 5. 工具配对：折叠时必须整轮一起走（不许留孤儿 tool 消息）----
  {
    std::vector<Message> h{sys("s")};
    h.push_back(u("跑一下 ls"));
    h.push_back(assistant_calls("run_shell"));
    h.push_back(tool_msg("run_shell", "a.txt\nb.txt"));
    h.push_back(a("有两个文件"));
    for (int i = 0; i < 4; ++i) { h.push_back(u(big_question("后面"))); h.push_back(a("ok")); }

    ContextManager cm(ContextConfig{/*max*/ 120, 0.5, /*keep*/ 2, 2048});
    const CompactReport r = cm.compact(h);
    check("配对：老工具轮被折叠了", r.did && r.summarized_turns >= 1, std::to_string(r.summarized_turns));
    check("配对：没有孤儿 tool 消息", !has_orphan_tool(h));
    check("配对：老轮的 tool 结果确实没了", count_role(h, "tool") == 0, std::to_string(count_role(h, "tool")));
  }

  // ---- 6. 检索类工具结果：不参与折叠（整轮豁免）----
  {
    std::vector<Message> h{sys("s")};
    h.push_back(u("读一下 a.txt"));
    h.push_back(assistant_calls("read_file"));
    h.push_back(tool_msg("read_file", std::string(3000, 'R')));
    h.push_back(a("读到了"));
    for (int i = 0; i < 6; ++i) { h.push_back(u(big_question("后面"))); h.push_back(a("ok")); }

    // 预算够大：折叠之后不进兜底，于是检索结果能保持原样
    ContextManager cm(ContextConfig{/*max*/ 2000, 0.5, /*keep*/ 1, 2048});
    const CompactReport r = cm.compact(h);
    const size_t ti = index_of_role(h, "tool");
    check("检索类：这一轮没被折叠", r.summarized_turns > 0, std::to_string(r.summarized_turns));
    check("检索类：结果原样保留", ti < h.size() && h[ti].content.size() == 3000,
          ti < h.size() ? std::to_string(h[ti].content.size()) : "no tool");
    check("检索类：没有孤儿", !has_orphan_tool(h));
    check("检索类：没触发兜底截断", r.truncated_tools == 0, std::to_string(r.truncated_tools));
  }

  // ---- 7. 非检索类的**老**工具结果：截断到 2KB（不丢消息）----
  {
    std::vector<Message> h{sys("s")};
    h.push_back(u("跑一下"));
    h.push_back(assistant_calls("run_shell"));
    h.push_back(tool_msg("run_shell", std::string(8000, 'X')));
    h.push_back(a("跑完了"));
    h.push_back(u("继续"));
    h.push_back(a("ok"));

    ContextManager cm(ContextConfig{/*max*/ 100000, 0.5, /*keep*/ 1, 2048});
    const CompactReport r = cm.compact(h);
    const size_t ti = index_of_role(h, "tool");
    const std::string tail = (ti < h.size() && h[ti].content.size() > 2048)
                                 ? h[ti].content.substr(2048, 60) : std::string("<no tail>");
    check("裁剪：老的非检索工具结果被截断", r.truncated_tools == 1, std::to_string(r.truncated_tools));
    check("裁剪：留下可读的截断标记",
          ti < h.size() && contains(h[ti].content, "[已截断：原始 8000 字节"), tail);
    check("裁剪：消息条数不变（只砍体积）", h.size() == 7, std::to_string(h.size()));
  }

  // ---- 8. 最近一轮的工具结果不动（模型正在用它推理）----
  {
    std::vector<Message> h{sys("s"), u("跑一下"), assistant_calls("run_shell"),
                           tool_msg("run_shell", std::string(8000, 'X'))};
    ContextManager cm(ContextConfig{/*max*/ 100000, 0.5, /*keep*/ 1, 2048});
    const CompactReport r = cm.compact(h);
    check("裁剪：最近一轮不截断", r.truncated_tools == 0 && h[3].content.size() == 8000,
          std::to_string(h[3].content.size()));
  }

  // ---- 9. 兜底：老轮全是检索类、压完还爆 -> 继续截断直到进预算 ----
  {
    std::vector<Message> h{sys("s")};
    for (int i = 0; i < 3; ++i) {
      h.push_back(u("读文件" + std::to_string(i)));
      h.push_back(assistant_calls("read_file"));
      h.push_back(tool_msg("read_file", std::string(9000, 'R')));
      h.push_back(a("读到了"));
    }
    h.push_back(u("最后一个问题"));
    h.push_back(a("ok"));

    ContextManager cm(ContextConfig{/*max*/ 500, 0.5, /*keep*/ 1, 2048});
    const CompactReport r = cm.compact(h);
    check("兜底：超预算时检索类也被截断", r.truncated_tools >= 1, std::to_string(r.truncated_tools));
    check("兜底：硬压回预算内", r.after_tokens <= 500, std::to_string(r.after_tokens));
    check("兜底：配对没被破坏", !has_orphan_tool(h));
  }

  // ---- 10. 结构不变量：system 在第 0 条、摘要不套娃、二次压缩不变大 ----
  {
    std::vector<Message> h{sys("S")};
    for (int i = 0; i < 8; ++i) { h.push_back(u(big_question("q" + std::to_string(i)))); h.push_back(a("a")); }
    ContextManager cm(ContextConfig{/*max*/ 150, 0.5, /*keep*/ 1, 2048});
    const CompactReport r1 = cm.compact(h);
    check("结构：压缩后第一条仍是 system", !h.empty() && h[0].role == "system");
    check("结构：第 1 条是摘要且只出现一次标记",
          h.size() > 1 && contains(h[1].content, "[压缩]") &&
          h[1].content.find("[压缩]", 1) == std::string::npos,
          h.size() > 1 ? h[1].content.substr(0, 40) : "");
    const size_t n1 = h.size();
    const size_t t1 = estimate_tokens(h);
    const CompactReport r2 = cm.compact(h);
    check("结构：二次压缩不变大", estimate_tokens(h) <= t1, std::to_string(t1) + " -> " +
          std::to_string(estimate_tokens(h)));
    check("结构：二次压缩不膨胀消息数", h.size() <= n1 + 1,
          std::to_string(n1) + " -> " + std::to_string(h.size()));
    (void)r1; (void)r2;
  }

  // ---- 11. 截断不能切在 UTF-8 码点中间：否则下一次 json::dump() 会抛 ----
  {
    // 2047 个 ASCII + 一个 3 字节汉字 + 换行：旧代码 resize(2048) 会留下半个汉字，
    // 再拼上 cut_marker 的 '\n'，就会报 invalid UTF-8 byte at index 2048: 0x0A。
    const std::string content = std::string(2047, 'X') + "你\n" + std::string(4000, 'Y');
    std::vector<Message> h{sys("s"), u("跑"), assistant_calls("run_shell"),
                           tool_msg("run_shell", content), a("done"), u("继续"), a("ok")};
    ContextManager cm(ContextConfig{/*max*/ 100000, 0.5, /*keep*/ 1, 2048});
    cm.compact(h);
    const size_t ti = index_of_role(h, "tool");
    bool valid = true;
    try {
      nlohmann::json j = h[ti].content;
      (void)j.dump();
    } catch (...) {
      valid = false;
    }
    check("裁剪：UTF-8 边界不被切断（dump 不抛）", valid,
          ti < h.size() ? std::to_string(h[ti].content.size()) : "no tool");
    check("裁剪：UTF-8 截断后仍带标记",
          ti < h.size() && contains(h[ti].content, "[已截断"));
  }

  std::printf("\n%s: %d\n", g_fail ? "FAILED" : "OK", g_fail);
  return g_fail ? 1 : 0;
}