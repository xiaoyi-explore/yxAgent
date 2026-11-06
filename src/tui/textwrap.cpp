#include "tui/textwrap.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <ftxui/screen/string.hpp>

namespace core {

std::vector<std::string> wrap_text(const std::string& text, int width) {
  if (width < 4) width = 4;
  std::string normalized;
  normalized.reserve(text.size());
  for (char c : text) {
    if (c == '\t') normalized += "    ";
    else if (c != '\r') normalized += c;
  }

  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t nl = normalized.find('\n', start);
    const std::string para =
        normalized.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (para.empty()) {
      out.push_back(" ");
    } else {
      std::string line;
      int used = 0;
      for (const std::string& glyph : ftxui::Utf8ToGlyphs(para)) {
        if (glyph.empty()) continue;
        const int w = std::max(1, ftxui::string_width(glyph));
        if (!line.empty() && used + w > width) {
          out.push_back(line);
          line.clear();
          used = 0;
        }
        line += glyph;
        used += w;
      }
      if (!line.empty()) out.push_back(line);
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
    if (start >= normalized.size()) break;  // 末尾换行不额外造一个空行
  }
  if (out.empty()) out.push_back(" ");
  return out;
}

}  // namespace core