#pragma once
// Policy（S8）—— 权限判定的唯一真相。
//
// 只回答一个问题：这次工具调用是 allow / ask / deny？
//   * read_file / write_file：路径必须在 workdir 内；越界 -> ask（never 模式直接 deny）
//   * run_shell：危险命令（rm -r/-f、sudo、解释器 -c、下载管道等）-> ask
//   * 其他：allow（未知工具交给注册表报错，不在这里吞掉）
//
// 它不是 OS 沙箱：不改变进程权限、不隔离文件系统。S8 是"策略闸门"，真正的沙箱
// 留给后续阶段；文档里必须这么说，别让人误以为 deny 之外还有一层内核保护。
#include "common/protocol.hpp"

#include <string>

namespace core {

using proto::json;

enum class ApprovalMode { Ask, Never, Allow };

enum class Permission { Allow, Ask, Deny };

struct PolicyResult {
  Permission permission = Permission::Allow;
  std::string reason;  // 人可读；ask/deny 时必须可行动
};

class Policy {
 public:
  Policy(std::string workdir, ApprovalMode mode);

  PolicyResult check(const std::string& tool, const json& args) const;

  ApprovalMode mode() const { return mode_; }
  static bool parse_mode(const std::string& text, ApprovalMode& out);
  static const char* mode_name(ApprovalMode mode);

 private:
  PolicyResult gate(Permission permission, std::string reason) const;
  bool within_workdir(const std::string& path) const;
  static std::string dangerous_reason(const std::string& command);
  static std::string canonical(const std::string& path);

  std::string workdir_;
  ApprovalMode mode_ = ApprovalMode::Ask;
};

}  // namespace core
