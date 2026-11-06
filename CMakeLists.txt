cmake_minimum_required(VERSION 3.16)
project(yxAgent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # clangd 需要（编辑器诊断用，不影响构建）
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# agent_common：协议工具，header-only。铁律：它绝不链接 HTTP / TUI 库。
add_library(agent_common INTERFACE)
target_include_directories(agent_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# httplib（vendored 单头文件） + OpenSSL：只属于 core 的 provider（S3a 引入）。
add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# S9：FTXUI（vendored，third_party/ftxui）。只构建库，不构建 examples/tests/docs。
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/ftxui EXCLUDE_FROM_ALL)

# core 引擎：agent-core 与 agent-cli(--embedded) 共用。
# --embedded 让 cli 在**链接层面**拿到 core（含 provider）——刻意的例外；
# 源码层面的铁律由 tests/test_s3a.py 的 layering 检查守着（cli 源码不得 include httplib/openssl）。
add_library(agent_core_lib STATIC
  src/core/core_engine.cpp
  src/core/openai_compat.cpp
  src/core/sse.cpp           # S3b：SSE 增量解析（本身不认识 HTTP，见下）
  src/core/tools.cpp         # S4：工具层（read_file / write_file / run_shell）
  src/core/context.cpp        # S6：预算与压缩（只认 vector<Message>，不认识协议/HTTP）
  src/core/session.cpp        # S7：会话落盘/恢复（只认 Message + json）
  src/core/policy.cpp)        # S8：allow/ask/deny（只认工具名 + 参数）
target_link_libraries(agent_core_lib PUBLIC agent_common httplib
                                      OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

add_executable(agent-core src/core/main.cpp)
target_link_libraries(agent-core PRIVATE agent_core_lib)

add_executable(agent-cli src/cli/main.cpp)
target_link_libraries(agent-cli PRIVATE agent_core_lib)

# S7：第二个前端（headless）。**只链接 agent_common**——不碰 core 静态库，
# 从链接层面证明它只靠协议活（源码层面的 grep 由 tests/test_s7.py 守着）。
add_executable(agent-run src/run/main.cpp)
target_link_libraries(agent-run PRIVATE agent_common)

# S9：FTXUI 前端。只链接 agent_common + FTXUI，不碰 agent_core_lib。
add_executable(agent-tui
  src/tui/main.cpp
  src/tui/core_client.cpp
  src/tui/tui_model.cpp
  src/tui/textwrap.cpp)
target_link_libraries(agent-tui PRIVATE agent_common component dom screen Threads::Threads)

# S3b：SSE 解析器的纯单元测试。它只编译 sse.cpp —— 不链接 httplib / OpenSSL，
# 等于顺手用构建系统证明"sse 不依赖网络库"（铁律的机器化检查）。
add_executable(sse_parser_test tests/sse_parser_test.cpp src/core/sse.cpp)
target_link_libraries(sse_parser_test PRIVATE agent_common)

# S6：压缩算法的纯单元测试（只编译 context.cpp，不链接网络/工具/引擎）。
add_executable(context_test tests/context_test.cpp src/core/context.cpp)
target_link_libraries(context_test PRIVATE agent_common)

# S9：TuiModel 的纯单元测试（不链接 FTXUI，也不链接 core）。
add_executable(tui_model_test tests/tui_model_test.cpp src/tui/tui_model.cpp)
target_link_libraries(tui_model_test PRIVATE agent_common)

# S9 fix：换行算法的纯单元测试（只依赖 FTXUI 的 string_width）。
add_executable(tui_wrap_test tests/tui_wrap_test.cpp src/tui/textwrap.cpp)
target_link_libraries(tui_wrap_test PRIVATE agent_common screen)

enable_testing()
add_test(NAME s1_handshake
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s1.py $<TARGET_FILE:agent-core>)
add_test(NAME s2_stream
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s2.py $<TARGET_FILE:agent-core> $<TARGET_FILE:agent-cli>)
add_test(NAME s3a_provider
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s3a.py $<TARGET_FILE:agent-core> $<TARGET_FILE:agent-cli>)
add_test(NAME s3b_sse_parser
         COMMAND sse_parser_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
add_test(NAME s3b_stream
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s3b.py $<TARGET_FILE:agent-core> $<TARGET_FILE:agent-cli>)
add_test(NAME s4_agent_tools
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s4.py $<TARGET_FILE:agent-core> $<TARGET_FILE:agent-cli>)
# S5：取消（turn.cancel）+ 增量合批。需要 fake_llm_sse.py 的 many / manyslow 两个模式。
add_test(NAME s5_cancel_batching
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s5.py $<TARGET_FILE:agent-core> $<TARGET_FILE:agent-cli>)
# S6：上下文预算与压缩（算法 + 端到端）。
add_test(NAME s6_context_unit COMMAND context_test)
add_test(NAME s6_context
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s6.py $<TARGET_FILE:agent-core> $<TARGET_FILE:agent-cli>)
# S7：会话持久化 / resume / headless 前端。
add_test(NAME s7_session
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s7.py $<TARGET_FILE:agent-core> $<TARGET_FILE:agent-cli> $<TARGET_FILE:agent-run>)
# S8：权限、审批与沙箱（Policy + approval.requested/respond）。
add_test(NAME s8_policy
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s8.py $<TARGET_FILE:agent-core> $<TARGET_FILE:agent-cli> $<TARGET_FILE:agent-run>)
# S9：TUI（模型单元测试 + pty 端到端）。
add_test(NAME s9_tui_model COMMAND tui_model_test)
add_test(NAME s9_tui_wrap COMMAND tui_wrap_test)
add_test(NAME s9_tui
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s9.py $<TARGET_FILE:agent-core> $<TARGET_FILE:agent-tui>)