#pragma once
// UTF-8 安全截断（header-only，agent_common 的一部分，零依赖）。
//
// 为什么需要它：nlohmann::json::dump() 遇到非法 UTF-8 会抛
//   [json.exception.type_error.316] invalid UTF-8 byte at index ...
// 而按字节 resize()/substr() 很容易把一个 3 字节汉字切成"半个"。
// 典型现场：tool result 被截到 2048 字节，第 2048 字节正好是中文的首字节，
// 后面又拼上 "\n[已截断...]"——0x0A 作为续字节不合法，下一次请求序列化就炸。
#include <cstddef>
#include <string>

namespace proto {

// 返回不超过 max_bytes、且停在完整 UTF-8 码点边界上的前缀长度。
// 遇到不完整/非法序列时宁可在它之前停下；不会返回半个码点。
inline size_t utf8_prefix_bytes(const std::string& s, size_t max_bytes) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 1;
    if ((c & 0x80) == 0x00) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    else { ++i; continue; }  // 非法首字节：按单字节推进，绝不越界

    if (i + len > s.size()) break;       // 尾部本身就不完整
    if (i + len > max_bytes) break;      // 再加这个码点会超预算
    i += len;
  }
  return i;
}

inline std::string utf8_truncate(const std::string& s, size_t max_bytes,
                                 const std::string& suffix = "") {
  const size_t n = utf8_prefix_bytes(s, max_bytes);
  if (n == s.size()) return s;
  return s.substr(0, n) + suffix;
}

}  // namespace proto
