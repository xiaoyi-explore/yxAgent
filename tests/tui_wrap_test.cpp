#include "tui/textwrap.hpp"

#include <ftxui/screen/string.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {
int fails = 0;
void check(const char* name, bool cond, const std::string& detail = "") {
  std::cout << (cond ? "PASS " : "FAIL ") << name;
  if (!cond && !detail.empty()) std::cout << "   " << detail;
  std::cout << "\n";
  if (!cond) ++fails;
}
}  // namespace

int main() {
  {
    const auto lines = core::wrap_text("abcdef", 4);
    check("ASCII 按宽度切", lines.size() == 2 && lines[0] == "abcd" && lines[1] == "ef");
  }
  {
    const auto lines = core::wrap_text("你好世界", 4);
    check("中文按显示宽度切（每个 2 格）",
          lines.size() == 2 && lines[0] == "你好" && lines[1] == "世界",
          lines.size() == 2 ? lines[0] + "/" + lines[1] : std::to_string(lines.size()));
  }
  {
    const auto lines = core::wrap_text("a\nb", 10);
    check("显式换行保留", lines.size() == 2 && lines[0] == "a" && lines[1] == "b");
  }
  {
    const auto lines = core::wrap_text("", 10);
    check("空文本仍占一行", lines.size() == 1 && lines[0] == " ");
  }
  {
    const std::string json = "{\"path\":\"/tmp/a-very-long-file-name.txt\",\"content\":\"hello\"}";
    const auto lines = core::wrap_text(json, 12);
    bool ok = !lines.empty();
    for (const std::string& line : lines)
      if (line != " " && ftxui::string_width(line) > 12) ok = false;
    check("长 JSON 不超宽", ok, std::to_string(lines.size()));
  }
  {
    const auto lines = core::wrap_text("abcdef", 1);  // clamp 到 4
    check("最小宽度 4", lines.size() == 2 && lines[0] == "abcd" && lines[1] == "ef");
  }
  return fails == 0 ? 0 : 1;
}