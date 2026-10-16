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

# core 引擎：agent-core 与 agent-cli(--embedded) 共用的那份逻辑。
# 注意：--embedded 让 cli 链接了 core 库，这是刻意的例外，理由见 docs/stage-s2.md。
add_library(agent_core_lib STATIC src/core/core_engine.cpp)
target_link_libraries(agent_core_lib PUBLIC agent_common)

add_executable(agent-core src/core/main.cpp)
target_link_libraries(agent-core PRIVATE agent_core_lib)

add_executable(agent-cli src/cli/main.cpp)
target_link_libraries(agent-cli PRIVATE agent_core_lib)

enable_testing()
add_test(NAME s1_handshake
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s1.py $<TARGET_FILE:agent-core>)
add_test(NAME s2_stream
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s2.py $<TARGET_FILE:agent-core> $<TARGET_FILE:agent-cli>)
