#include "core/policy.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <utility>
#include <vector>

namespace core {
namespace {

namespace fs = std::filesystem;

struct Rule {
  std::regex pattern;
  const char* reason;
};

const std::vector<Rule>& rules() {
  static const std::vector<Rule> kRules = {
      {std::regex(R"((^|[;&|]+\s*)(sudo|su|doas)\b)"), "privilege escalation (sudo/su)"},
      {std::regex(R"(\brm\s+(-[a-zA-Z]*[rf][a-zA-Z]*\s+)+)"), "recursive/forced delete (rm -r/-f)"},
      {std::regex(R"(\b(mkfs(\.\w+)?|fdisk|parted|shutdown|reboot|poweroff)\b)"), "system-level command"},
      {std::regex(R"(\bdd\s+[^;&|]*\bof=\/dev\/)"), "raw device write (dd of=/dev/...)"},
      {std::regex(R"(\b(bash|sh|zsh|dash|fish)\s+-[a-zA-Z]*c\b)"), "interpreter + code (bash/sh -c)"},
      {std::regex(R"(\b(python|python3|node|perl|ruby|php)\s+-[a-zA-Z]*[ce]\b)"), "interpreter + code (python -c / node -e)"},
      {std::regex(R"(\b(curl|wget)\b[^;&|]*\|\s*(bash|sh|zsh)\b)"), "download piped to shell"},
      {std::regex(R"((^|[;&|]+\s*)>{1,2}\s*/)"), "redirect to root path"},
      {std::regex(R"(\bchmod\s+(-R\s+)?[0-7]{3,4}\s+/)"), "chmod on root path"},
      {std::regex(R"(\bchown\s+[^;&|]*\s+/)"), "chown on root path"},
  };
  return kRules;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

}  // namespace

Policy::Policy(std::string workdir, ApprovalMode mode) : mode_(mode) {
  if (workdir.empty()) {
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    workdir = ec ? "." : cwd.string();
  }
  workdir_ = canonical(workdir);
}

std::string Policy::canonical(const std::string& path) {
  std::error_code ec;
  const fs::path p = fs::weakly_canonical(fs::path(path), ec);
  if (!ec) return p.string();
  const fs::path a = fs::absolute(fs::path(path), ec);
  if (!ec) return a.lexically_normal().string();
  return path;
}

bool Policy::within_workdir(const std::string& path) const {
  if (path.empty()) return true;  // 让工具自己报 missing path，不在这里变成审批
  fs::path target(path);
  if (target.is_relative()) target = fs::path(workdir_) / target;
  const std::string full = canonical(target.string());

  std::error_code ec;
  const fs::path rel = fs::relative(fs::path(full), fs::path(workdir_), ec);
  if (ec) return full == workdir_;
  const std::string s = rel.generic_string();
  if (s.empty() || s == ".") return true;
  return s != ".." && s.rfind("../", 0) != 0;
}

std::string Policy::dangerous_reason(const std::string& command) {
  const std::string cmd = lower(command);
  for (const Rule& rule : rules()) {
    if (std::regex_search(cmd, rule.pattern))
      return std::string("dangerous shell command: ") + rule.reason;
  }
  return {};
}

PolicyResult Policy::gate(Permission permission, std::string reason) const {
  if (permission == Permission::Allow) return {Permission::Allow, std::move(reason)};
  if (mode_ == ApprovalMode::Never)
    return {Permission::Deny, "approval policy=never: " + reason};
  if (mode_ == ApprovalMode::Allow)
    return {Permission::Allow, "approval policy=allow: " + reason};
  return {Permission::Ask, std::move(reason)};
}

PolicyResult Policy::check(const std::string& tool, const json& args) const {
  if (tool == "read_file" || tool == "write_file") {
    const std::string path = (args.contains("path") && args["path"].is_string())
                                 ? args["path"].get<std::string>() : std::string();
    if (path.empty() || within_workdir(path)) return {Permission::Allow, {}};
    return gate(Permission::Ask, "path is outside the working directory: " + path);
  }

  if (tool == "run_shell") {
    const std::string command = (args.contains("command") && args["command"].is_string())
                                    ? args["command"].get<std::string>() : std::string();
    const std::string danger = dangerous_reason(command);
    if (!danger.empty()) return gate(Permission::Ask, danger);
    return {Permission::Allow, {}};
  }

  // 未知工具：交给 ToolRegistry 回 ok=false，不在策略层吞掉。
  return {Permission::Allow, {}};
}

bool Policy::parse_mode(const std::string& text, ApprovalMode& out) {
  if (text == "ask") { out = ApprovalMode::Ask; return true; }
  if (text == "never" || text == "deny") { out = ApprovalMode::Never; return true; }
  if (text == "allow" || text == "auto") { out = ApprovalMode::Allow; return true; }
  return false;
}

const char* Policy::mode_name(ApprovalMode mode) {
  switch (mode) {
    case ApprovalMode::Ask: return "ask";
    case ApprovalMode::Never: return "never";
    case ApprovalMode::Allow: return "allow";
  }
  return "ask";
}

}  // namespace core