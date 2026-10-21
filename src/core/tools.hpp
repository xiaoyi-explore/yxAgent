#pragma once
// 工具层（S4）——core 的"手脚"。
//
// 为什么工具执行在 core 而不是前端：权限判定必须与执行同进程（docs/responsibilities.md 铁律）。
// 前端只看到 tool.call / tool.result 两个事件，永远不执行任何东西。
//
// 本阶段的边界（刻意不做）：
//   * 不做路径沙箱、不做"要不要问用户"——那是 S8 的审批层，这里只把 workdir 当作相对路径的锚点；
//   * 不做并发：一个 turn 里的工具调用是顺序执行的（ReAct 循环本来就是一步接一步）。
#include "core/provider.hpp"

#include <memory>
#include <string>
#include <vector>

namespace core {

// 一次工具执行的结果。
// 注意：**失败也要喂回模型**（"ERROR: ..." 是模型自我纠正的唯一信息来源），
// 所以 ok/error 是给事件用的结构化字段，output 是给人/模型看的文本。
struct ToolResult {
  bool ok = false;
  std::string output;
  std::string error;

  static ToolResult success(std::string out) { return ToolResult{true, std::move(out), ""}; }
  static ToolResult failure(std::string err) { return ToolResult{false, "", std::move(err)}; }
};

class Tool {
 public:
  virtual ~Tool() = default;
  virtual std::string name() const = 0;
  virtual std::string description() const = 0;
  virtual json parameters() const = 0;  // JSON Schema（给模型看）
  // const：工具本身无状态（workdir 是构造时定死的），execute() 拿到的是 const Tool*
  virtual ToolResult run(const json& args) const = 0;

  ToolSpec spec() const { return ToolSpec{name(), description(), parameters()}; }
};

// 注册表：名字 -> 工具。execute() 对未知工具返回 failure 而不是抛异常
// （模型的幻觉不该让 core 崩掉）。
class ToolRegistry {
 public:
  void add(std::unique_ptr<Tool> tool);
  bool empty() const { return tools_.empty(); }
  size_t size() const { return tools_.size(); }
  std::string names() const;  // "read_file,run_shell"（给 usage/报错用）

  std::vector<ToolSpec> specs() const;
  const Tool* find(const std::string& name) const;
  ToolResult execute(const std::string& name, const json& args);

 private:
  std::vector<std::unique_ptr<Tool>> tools_;
};

// ---- 内置三件套（S4 的全部工具）----
// workdir：相对路径的锚点，也是 run_shell 的工作目录（空 = 进程当前目录）。
std::unique_ptr<Tool> make_read_file_tool(std::string workdir);
std::unique_ptr<Tool> make_write_file_tool(std::string workdir);
std::unique_ptr<Tool> make_run_shell_tool(std::string workdir);

// 按名字建一个内置工具；不认识的名字返回 nullptr（core 启动时据此报错）
std::unique_ptr<Tool> make_builtin_tool(const std::string& name, const std::string& workdir);

// 解析 --tools 的取值：all = 三件套；none/空 = 不给；否则按逗号切分。
// 放在这里而不是 main.cpp：core 和 cli 都要用同一份规则（两边理解不一致就是 bug）。
std::vector<std::string> parse_tool_list(const std::string& value);

}  // namespace core
