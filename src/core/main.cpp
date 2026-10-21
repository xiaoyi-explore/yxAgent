// agent-core —— core 进程的入口：解析参数，把引擎挂到 stdio 上。
// 从 S2 起真正的逻辑都在 core::CoreEngine 里——这样 cli 的 --embedded 才能直接调用它。
#include "core/core_engine.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using proto::json;

namespace {

void usage() {
  std::cerr << "usage: agent-core [--log-raw FILE] [--demo-delay-ms N]\n"
               "                  [--base-url URL] [--model NAME] [--api-key KEY] [--provider-timeout-ms N]\n"
               "  --log-raw FILE      把双向每一帧记到文件（--> 去向 core，<-- 来自 core）\n"
               "  --demo-delay-ms N   假模型每吐一个字等待的毫秒数（默认 30，0 = 全速）\n"
               "  --base-url URL      模型端点（默认 https://api.deepseek.com）\n"
               "  --model NAME        模型名（默认 deepseek-chat）\n"
               "  --api-key KEY       API key；更安全的方式是环境变量 DEEPSEEK_API_KEY（argv 在 ps 里可见）\n"
               "  --provider-timeout-ms N  读超时（默认 120000）\n"
               "  --no-stream         不用 SSE，回到 S3a 的整段返回（默认是流式）\n"
               "  --tools LIST        给模型装上手脚：all 或逗号分隔（read_file,write_file,run_shell）。默认不给\n"
               "  --workdir DIR       工具的相对路径锚点，也是 run_shell 的工作目录（默认当前目录）\n"
               "  --max-steps N       一个 turn 里最多几次模型调用（默认 8，防死循环）\n";
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
    else if (opt_value(argc, argv, i, "--base-url", v)) opts.base_url = v;
    else if (opt_value(argc, argv, i, "--model", v)) opts.model = v;
    else if (opt_value(argc, argv, i, "--api-key", v)) opts.api_key = v;
    else if (opt_value(argc, argv, i, "--provider-timeout-ms", v))
      opts.provider_timeout_ms = std::max(1, std::atoi(v.c_str()));
    else if (a == "--no-stream") opts.stream = false;   // 回到 S3a 的整段返回
    else if (a == "--stream") opts.stream = true;
    else if (opt_value(argc, argv, i, "--stream", v)) opts.stream = (v != "false" && v != "0");
    else if (opt_value(argc, argv, i, "--tools", v)) opts.tools = core::parse_tool_list(v);
    else if (opt_value(argc, argv, i, "--workdir", v)) opts.workdir = v;
    else if (opt_value(argc, argv, i, "--max-steps", v)) opts.max_steps = std::max(1, std::atoi(v.c_str()));
    else { std::cerr << "agent-core: unknown argument: " << a << "\n"; usage(); return 1; }
  }

  // key 优先 --api-key，其次环境变量。环境变量更安全：argv 对同机所有用户可见。
  // 两个变量同时存在时，选谁都是陷阱——必须吵，不能静默选一个。
  if (opts.api_key.empty()) {
    const char* deepseek_key = std::getenv("DEEPSEEK_API_KEY");
    const char* openai_key = std::getenv("OPENAI_API_KEY");
    if (deepseek_key != nullptr && openai_key != nullptr && std::string(deepseek_key) != openai_key) {
      std::cerr << "[core] warning: both DEEPSEEK_API_KEY and OPENAI_API_KEY are set and differ; using "
                   "DEEPSEEK_API_KEY. Unset one, or pass --api-key explicitly.\n";
    }
    if (deepseek_key != nullptr) opts.api_key = deepseek_key;
    else if (openai_key != nullptr) opts.api_key = openai_key;
  }

  // 模型名默认是 deepseek-chat；若端点指到别家，这会是一个静默的坑（OpenAI 会报 model not found）。
  if (opts.model.empty() && !opts.base_url.empty() &&
      opts.base_url.find("deepseek") == std::string::npos) {
    std::cerr << "[core] warning: --model not given, defaulting to deepseek-chat; pass --model for "
              << opts.base_url << " (e.g. gpt-4o-mini, qwen-max...)\n";
  }

  core::CoreEngine engine(std::move(opts));
  return engine.run(std::cin, std::cout);
}
