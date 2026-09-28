# apex 纯函数单测：helpers/turn 不依赖 llama。
# 文件名遵守 names.md（无 _/-）。

include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
if (NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(json)
endif ()


add_executable(apexdecide decide.cpp)
target_compile_features(apexdecide PRIVATE cxx_std_17)
add_test(NAME apexdecide COMMAND apexdecide)
set_tests_properties(apexdecide PROPERTIES LABELS "apex;unit" TIMEOUT 30)

add_executable(apexhelpers helpers.cpp)
target_compile_features(apexhelpers PRIVATE cxx_std_17)
target_include_directories(apexhelpers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/apex)
target_link_libraries(apexhelpers PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME apexhelpers COMMAND apexhelpers)
set_tests_properties(apexhelpers PROPERTIES LABELS "apex;unit" TIMEOUT 30)

add_executable(apexturn turn.cpp)
target_compile_features(apexturn PRIVATE cxx_std_17)
target_include_directories(apexturn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/apex)
target_link_libraries(apexturn PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME apexturn COMMAND apexturn)
set_tests_properties(apexturn PROPERTIES LABELS "apex;unit" TIMEOUT 30)

add_executable(apexfuse fuse.cpp)
target_compile_features(apexfuse PRIVATE cxx_std_17)
target_include_directories(apexfuse PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/apex)
target_link_libraries(apexfuse PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME apexfuse COMMAND apexfuse)
set_tests_properties(apexfuse PROPERTIES LABELS "apex;unit" TIMEOUT 30)

add_executable(apexworker worker.cpp)
target_compile_features(apexworker PRIVATE cxx_std_17)
add_test(NAME apexworker COMMAND apexworker)
set_tests_properties(apexworker PROPERTIES LABELS "apex;unit" TIMEOUT 30)

add_executable(apexrules rules.cpp)
target_compile_features(apexrules PRIVATE cxx_std_17)
target_include_directories(apexrules PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/apex)
target_link_libraries(apexrules PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME apexrules COMMAND apexrules)
set_tests_properties(apexrules PROPERTIES LABELS "apex;unit" TIMEOUT 30)

add_executable(apexshadow shadow.cpp)
target_compile_features(apexshadow PRIVATE cxx_std_17)
add_test(NAME apexshadow COMMAND apexshadow)
set_tests_properties(apexshadow PROPERTIES LABELS "apex;unit" TIMEOUT 30)

# 三个纯逻辑测试目标不经过 setup_core_target，需手动接入 sanitizer/coverage，
# 否则 ASan/UBSan/TSan 矩阵与 lcov 覆盖率都覆盖不到 apex 纯逻辑。
foreach(_apex_tgt apexdecide apexhelpers apexturn apexfuse apexworker apexrules apexshadow)
    usearch_apply_sanitizers(${_apex_tgt})
    # 这些测试全部用 assert 校验；CI 用 RelWithDebInfo(-DNDEBUG) 会把断言剥成空操作，
    # 导致测试"跑过但什么都没断言"。这里为测试目标关掉 NDEBUG，保证断言在任何构建档都生效。
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${_apex_tgt} PRIVATE -UNDEBUG)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        target_compile_options(${_apex_tgt} PRIVATE /UNDEBUG)
    endif ()
    if (USEARCH_APEX_WERROR)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${_apex_tgt} PRIVATE -Wall -Wextra -Werror)
        elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
            target_compile_options(${_apex_tgt} PRIVATE /WX)
        endif ()
    endif ()
endforeach()
