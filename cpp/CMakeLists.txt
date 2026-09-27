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
    # 覆盖 setup_target 里可能叠上的 TSan/独占 ASAN，保证 fuzzer 链接集完整。
    if (USEARCH_BUILD_TEST_CPP)
        add_test(NAME fuzz_index_smoke COMMAND fuzz_index -max_total_time=20 -timeout=5)
        set_tests_properties(fuzz_index_smoke PROPERTIES LABELS "fuzz;memory" TIMEOUT 120)
    endif ()
endif ()
