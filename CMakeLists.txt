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

# core 引擎：agent-core 与 agent-cli(--embedded) 共用。
# --embedded 让 cli 在**链接层面**拿到 core（含 provider）——刻意的例外；
# 源码层面的铁律由 tests/test_s3a.py 的 layering 检查守着（cli 源码不得 include httplib/openssl）。
add_library(agent_core_lib STATIC
  src/core/core_engine.cpp
  src/core/openai_compat.cpp
  src/core/sse.cpp)          # S3b：SSE 增量解析（本身不认识 HTTP，见下）
target_link_libraries(agent_core_lib PUBLIC agent_common httplib
                                      OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

add_executable(agent-core src/core/main.cpp)
target_link_libraries(agent-core PRIVATE agent_core_lib)

add_executable(agent-cli src/cli/main.cpp)
target_link_libraries(agent-cli PRIVATE agent_core_lib)

# S3b：SSE 解析器的纯单元测试。它只编译 sse.cpp —— 不链接 httplib / OpenSSL，
# 等于顺手用构建系统证明"sse 不依赖网络库"（铁律的机器化检查）。
add_executable(sse_parser_test tests/sse_parser_test.cpp src/core/sse.cpp)
target_link_libraries(sse_parser_test PRIVATE agent_common)

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
