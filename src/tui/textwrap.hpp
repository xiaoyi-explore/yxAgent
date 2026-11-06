#pragma once
// 按显示宽度换行（中文算 2 格）。FTXUI 的 text() 不换行、paragraph() 只按空格切，
// 中文和长 JSON 都会横向溢出，所以单独抽出来，既能复用也能单测。
#include <string>
#include <vector>

namespace core {

std::vector<std::string> wrap_text(const std::string& text, int width);

}  // namespace core
