# 单元 / NumKong 门禁测试（源码仅放本目录，见 AGENTS.md）。
if (NOT USEARCH_BUILD_TEST_CPP)
    return()
endif ()

include(CTest)
enable_testing()

add_executable(test_cpp unit.cpp)
setup_core_target(test_cpp)
add_test(NAME test_cpp COMMAND test_cpp)
set_tests_properties(test_cpp PROPERTIES LABELS "unit;memory" TIMEOUT 600)

add_executable(test_correctness correctness.cpp)
setup_core_target(test_correctness)
add_test(NAME test_correctness COMMAND test_correctness)
set_tests_properties(test_correctness PROPERTIES LABELS "unit;correctness" TIMEOUT 120)

add_executable(testoom oom.cpp)
setup_core_target(testoom)
add_test(NAME testoom COMMAND testoom)
set_tests_properties(testoom PROPERTIES LABELS "unit;correctness;oom" TIMEOUT 60)

add_executable(test_batch batch.cpp)
setup_core_target(test_batch)
add_test(NAME test_batch COMMAND test_batch)
set_tests_properties(test_batch PROPERTIES LABELS "unit;correctness;batch" TIMEOUT 60)

if (TARGET usearch_c)
    add_executable(test_capi capi.cpp)
    setup_core_target(test_capi)
    target_link_libraries(test_capi PRIVATE usearch_c)
    add_test(NAME test_capi COMMAND test_capi)
    set_tests_properties(test_capi PROPERTIES LABELS "unit;correctness;capi" TIMEOUT 60)
endif ()

add_executable(test_perf perf.cpp)
setup_perf_target(test_perf)
add_test(NAME test_perf COMMAND test_perf)
set_tests_properties(test_perf PROPERTIES LABELS "perf" TIMEOUT 120)

if (USEARCH_USE_NUMKONG)
    add_executable(test_numkong_dispatch dispatch.cpp)
    setup_core_target(test_numkong_dispatch)
    add_test(NAME test_numkong_dispatch COMMAND test_numkong_dispatch)
    set_tests_properties(test_numkong_dispatch PROPERTIES LABELS "unit;numkong" TIMEOUT 120)

    add_executable(test_numkong_diff differential.cpp)
    setup_core_target(test_numkong_diff)
    add_test(NAME test_numkong_diff COMMAND test_numkong_diff)
    set_tests_properties(test_numkong_diff PROPERTIES LABELS "unit;numkong" TIMEOUT 180)
endif ()

target_include_directories(test_cpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../stringzilla/include)

# 导出动态符号表，便于崩溃处理解析栈帧。
set_target_properties(test_cpp PROPERTIES ENABLE_EXPORTS ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(test_cpp PRIVATE -Wno-vla -Wno-unused-function -Wno-cast-function-type)
endif ()

# apex 纯逻辑（不拉 llama/httplib）；始终可编进 test 树。
add_subdirectory(apex)
