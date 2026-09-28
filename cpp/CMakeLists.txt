# 核心附属目标：基准与 fuzz（网关见 tools/apex）。

if (USEARCH_BUILD_BENCH_CPP)
    include(clipp)
    add_executable(bench_cpp bench.cpp)
    setup_target(bench_cpp)
    target_include_directories(bench_cpp PRIVATE ${clipp_SOURCE_DIR}/include)
endif ()

if (USEARCH_BUILD_FUZZ)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "USEARCH_BUILD_FUZZ requires Clang with libFuzzer")
    endif ()
    add_executable(fuzz_index fuzz_index.cpp)
    setup_target(fuzz_index)
    target_compile_options(fuzz_index PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(fuzz_index PRIVATE -fsanitize=fuzzer,address,undefined)

    # 文件头/metadata 解析专用 harness（不 load 整图）
    add_executable(fuzz_format fuzz_format.cpp)
    setup_target(fuzz_format)
    target_compile_options(fuzz_format PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(fuzz_format PRIVATE -fsanitize=fuzzer,address,undefined)

    # apex agent result 解析 harness：render.hpp 纯头 + nlohmann::json，不链 llama/sqlite。
    if (NOT TARGET nlohmann_json::nlohmann_json)
        include(FetchContent)
        set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
        set(JSON_Install OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            json
            GIT_REPOSITORY https://github.com/nlohmann/json
            GIT_TAG v3.11.3
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(json)
    endif ()
    add_executable(fuzz_agentjson fuzz_agentjson.cpp)
    setup_target(fuzz_agentjson)
    target_include_directories(fuzz_agentjson PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools/apex)
    target_link_libraries(fuzz_agentjson PRIVATE nlohmann_json::nlohmann_json)
    target_compile_options(fuzz_agentjson PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(fuzz_agentjson PRIVATE -fsanitize=fuzzer,address,undefined)

    if (USEARCH_BUILD_TEST_CPP)
        add_test(NAME fuzz_index_smoke COMMAND fuzz_index -max_total_time=20 -timeout=5)
        set_tests_properties(fuzz_index_smoke PROPERTIES LABELS "fuzz;memory" TIMEOUT 120)
        add_test(NAME fuzz_format_smoke COMMAND fuzz_format -max_total_time=20 -timeout=5)
        set_tests_properties(fuzz_format_smoke PROPERTIES LABELS "fuzz;memory;format" TIMEOUT 120)
        add_test(NAME fuzz_agentjson_smoke COMMAND fuzz_agentjson -max_total_time=20 -timeout=5)
        set_tests_properties(fuzz_agentjson_smoke PROPERTIES LABELS "fuzz;memory;apex" TIMEOUT 120)
    endif ()
endif ()
