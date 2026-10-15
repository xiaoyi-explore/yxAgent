cmake_minimum_required(VERSION 3.16)
project(yxAgent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # clangd 需要（编辑器诊断用，不影响构建）
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# agent_common：协议工具，header-only。铁律：绝不链接 HTTP / TUI 库。
add_library(agent_common INTERFACE)
target_include_directories(agent_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(agent-core src/core/main.cpp)
target_link_libraries(agent-core PRIVATE agent_common)

add_executable(agent-cli src/cli/main.cpp)
target_link_libraries(agent-cli PRIVATE agent_common)

enable_testing()
add_test(NAME s1_handshake
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s1.py $<TARGET_FILE:agent-core>)
