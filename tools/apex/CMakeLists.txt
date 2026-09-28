# LLM 前置网关（HTTP / MCP），非 USearch 核心库。
# 启用：-DUSEARCH_BUILD_API=ON
# SQLite 存储实现在 tools/sqlite/（与网关源码分目录）。

if (NOT USEARCH_BUILD_API)
    return()
endif ()

include(httplib)
include(sqlite)
include(llama)
include(toml)
enable_language(C)

add_executable(api
    api.cpp
    dense.cpp
    sq8.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sqlite/sql.cpp
    text.cpp
    http.cpp
    mcp.cpp
    decide.cpp
    gate.cpp
    upstream.cpp
)
# 服务端二进制：可移植 core（-O2），禁止 -march=native。
setup_core_target(api)
target_compile_features(api PRIVATE cxx_std_17)
# sql.cpp 在 tools/sqlite/，仍依赖本目录 api.hpp
target_include_directories(api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(api PRIVATE httplib::httplib nlohmann_json::nlohmann_json tomlplusplus::tomlplusplus sqlite3 llama)
if (WIN32)
    target_link_libraries(api PRIVATE ws2_32)
    target_compile_definitions(api PRIVATE _WIN32_WINNT=0x0A00)
endif ()
if (UNIX AND NOT APPLE)
    target_link_libraries(api PRIVATE pthread dl)
endif ()
