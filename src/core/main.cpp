// agent-core —— core 进程的入口：解析参数，把引擎挂到 stdio 上。
// 从 S2 起真正的逻辑都在 core::CoreEngine 里——这样 cli 的 --embedded 才能直接调用它。
#include "core/core_engine.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

using proto::json;

namespace {

void usage() {
  std::cerr << "usage: agent-core [--log-raw FILE] [--demo-delay-ms N]\n"
               "  --log-raw FILE      把双向每一帧记到文件（--> 去向 core，<-- 来自 core）\n"
               "  --demo-delay-ms N   假模型每吐一个字等待的毫秒数（默认 30，0 = 全速）\n";
}

// 取开关的值：支持 --k=v 与 --k v 两种写法；命中返回 true
bool opt_value(int argc, char** argv, int& i, const char* key, std::string& out) {
  const std::string a = argv[i];
  const std::string k = key;
  if (a == k) {
    if (i + 1 >= argc) return false;
    out = argv[++i];
    return true;
  }
  if (a.rfind(k + "=", 0) == 0) {
    out = a.substr(k.size() + 1);
    return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);

  core::Options opts;
  for (int i = 1; i < argc; ++i) {
    std::string v;
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") { usage(); return 0; }
    else if (opt_value(argc, argv, i, "--log-raw", v)) opts.log_raw_path = v;
    else if (opt_value(argc, argv, i, "--demo-delay-ms", v)) opts.demo_delay_ms = std::max(0, std::atoi(v.c_str()));
    else { std::cerr << "agent-core: unknown argument: " << a << "\n"; usage(); return 1; }
  }

  core::CoreEngine engine(std::move(opts));
  return engine.run(std::cin, std::cout);
}
