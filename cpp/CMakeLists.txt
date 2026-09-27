if (USEARCH_BUILD_TEST_CPP)
    add_executable(test_cpp test.cpp)
    setup_target(test_cpp)
    include(CTest)
    enable_testing()
    add_test(NAME test_cpp COMMAND test_cpp)
    set_tests_properties(test_cpp PROPERTIES LABELS "unit;memory" TIMEOUT 600)

    target_include_directories(test_cpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../stringzilla/include)

    # Export the dynamic symbol table so `backtrace_symbols` / `std::stacktrace`
    # can resolve function names when the in-test crash handler fires.
    set_target_properties(test_cpp PROPERTIES ENABLE_EXPORTS ON)

    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(test_cpp PRIVATE -Wno-vla -Wno-unused-function -Wno-cast-function-type)
    endif ()
endif ()

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

if (USEARCH_BUILD_API)
    include(httplib)
    include(sqlite)
    include(llama)
    enable_language(C)
    add_executable(api
        api.cpp
        dense.cpp
        sq8.cpp
        sql.cpp
        text.cpp
        http.cpp
        mcp.cpp
    )
    setup_target(api)
    target_compile_features(api PRIVATE cxx_std_17)
    target_include_directories(api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(api PRIVATE httplib::httplib nlohmann_json::nlohmann_json sqlite3 llama)
    if (WIN32)
        target_link_libraries(api PRIVATE ws2_32)
        target_compile_definitions(api PRIVATE _WIN32_WINNT=0x0A00)
    endif ()
    if (UNIX AND NOT APPLE)
        target_link_libraries(api PRIVATE pthread dl)
    endif ()
endif ()
