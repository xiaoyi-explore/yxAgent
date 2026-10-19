// SSE 解析器的纯单元测试（S3b）。
//
// 它要证明的核心事实只有一句：**把同一段字节按任意位置切开，解析结果必须一模一样**。
// 于是对每个 fixture 都做"穷举切点"：从 1 字节一喂到一次喂完，逐个比对事件序列。
// 这条测试比任何 HTTP 级测试都更狠——TCP/TLS 怎么分片它都能模拟，而且完全确定性。
#include "core/sse.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(const std::string& name, bool ok, const std::string& detail = "") {
  std::printf(ok ? "PASS %s\n" : "FAIL %s   %s\n", name.c_str(), detail.c_str());
  if (!ok) ++failures;
}

using Event = std::pair<std::string, std::string>;  // (event, data)
using Stream = std::vector<Event>;

Stream parse(const std::string& bytes, size_t chunk) {
  Stream out;
  core::SseParser p([&](const core::SseEvent& ev) { out.emplace_back(ev.event, ev.data); });
  if (chunk == 0) {
    p.feed(bytes.data(), bytes.size());
  } else {
    for (size_t i = 0; i < bytes.size(); i += chunk)
      p.feed(bytes.data() + i, std::min(chunk, bytes.size() - i));
  }
  p.flush();
  return out;
}

bool same(const Stream& a, const Stream& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i]) return false;
  return true;
}

std::string describe(const Stream& s) {
  std::ostringstream o;
  o << s.size() << " events";
  for (size_t i = 0; i < s.size() && i < 3; ++i)
    o << " [" << i << ":" << s[i].first << "=" << s[i].second.substr(0, 24) << "]";
  return o.str();
}

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}

std::string to_crlf(const std::string& s) {
  std::string out;
  out.reserve(s.size() + s.size() / 16);
  for (const char c : s) {
    if (c == '\n') out += "\r\n";
    else out += c;
  }
  return out;
}

// 穷举切点：1..N 字节，外加几个"一次喂完"的粗粒度
void exhaustive(const std::string& label, const std::string& bytes) {
  const Stream ref = parse(bytes, 0);
  check(label + ": 基准可解析（" + describe(ref) + "）", !ref.empty());

  std::vector<size_t> sizes;
  for (size_t k = 1; k <= std::min<size_t>(bytes.size(), 40); ++k) sizes.push_back(k);
  for (size_t k : {size_t{64}, size_t{97}, size_t{256}, size_t{1024}}) sizes.push_back(k);

  size_t bad = 0;
  std::string first_bad;
  for (const size_t k : sizes) {
    const Stream got = parse(bytes, k);
    if (!same(got, ref)) {
      if (bad == 0) first_bad = "chunk=" + std::to_string(k) + " -> " + describe(got);
      ++bad;
    }
  }
  check(label + ": 任意切分结果一致（" + std::to_string(sizes.size()) + " 种切法）", bad == 0, first_bad);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string fixtures = (argc > 1) ? argv[1] : "tests/fixtures";

  // ---- 1. fixture 文件：穷举切分（含 JSON 中间切、UTF-8 中间切）----
  const std::vector<std::string> names = {"sse_basic.sse", "sse_comment.sse", "sse_nousage.sse",
                                          "sse_nodone.sse"};
  for (const std::string& name : names) {
    const std::string bytes = read_file(fixtures + "/" + name);
    if (bytes.empty()) {
      check(name + ": fixture 存在且非空", false, fixtures + "/" + name);
      continue;
    }
    exhaustive(name, bytes);
    exhaustive(name + " (CRLF)", to_crlf(bytes));
  }

  // ---- 2. 手写边界：注释 / 多行 data / 纯 CR / 半个 CRLF / 无空行收尾 ----
  {
    const Stream s = parse(": keep-alive\n\ndata: a\n\n", 0);
    check("注释行被忽略", s.size() == 1 && s[0].second == "a", describe(s));
  }
  {
    const Stream s = parse("data: a\ndata: b\n\n", 0);
    check("多行 data 用换行拼接", s.size() == 1 && s[0].second == "a\nb", describe(s));
  }
  {
    const Stream s = parse("data: a\r\r", 0);
    check("纯 CR 行尾", s.size() == 1 && s[0].second == "a", describe(s));
  }
  {
    // CR 落在 chunk 末尾，LF 在下一个 chunk —— 最容易写错的一种切法
    Stream out;
    core::SseParser p([&](const core::SseEvent& ev) { out.emplace_back(ev.event, ev.data); });
    p.feed("data: a\r", 8);
    p.feed("\n\r\n", 3);
    p.flush();
    check("CR 与 LF 被切在两个 chunk 里", out.size() == 1 && out[0].second == "a", describe(out));
  }
  {
    const Stream s = parse("data: a\n", 0);  // 没有空行收尾，靠 flush()
    check("末尾没有空行也能结算（flush）", s.size() == 1 && s[0].second == "a", describe(s));
  }
  {
    const Stream s = parse("", 0);
    check("空流不派发任何事件", s.empty(), describe(s));
  }
  {
    const Stream s = parse("data: [DONE]\n\n", 0);
    check("[DONE] 作为一个事件原样交给上层", s.size() == 1 && s[0].second == "[DONE]", describe(s));
  }
  {
    // 一个事件只装一个字节：UTF-8 码点被切碎后，逐字节还原必须还是原文
    const std::string text = u8"你好，世界";
    std::string bytes;
    for (size_t i = 0; i < text.size(); ++i) {
      bytes += "data: ";
      bytes += text[i];
      bytes += "\n\n";
    }
    std::string joined;
    for (const Event& e : parse(bytes, 1)) joined += e.second;
    check("UTF-8 码点被切碎后仍能拼回原样", joined == text, joined);
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("FAILED: %d\n", failures);
    return 1;
  }
  std::printf("ALL S3B SSE PARSER TESTS PASSED\n");
  return 0;
}
