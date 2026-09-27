include(FetchContent)
# toml++：C++ 侧顶级 TOML 解析（header-only / INTERFACE）。进程配置用 TOML；HTTP/MCP 线协议仍用 JSON。
set(TOMLPLUSPLUS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TOMLPLUSPLUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tomlplusplus)
