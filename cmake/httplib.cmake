include(FetchContent)
set(HTTPLIB_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
    GIT_TAG v0.18.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(httplib)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)
