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

if (USEARCH_APEX_WERROR)
    foreach(_apex_tgt apexdecide apexhelpers apexturn)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${_apex_tgt} PRIVATE -Wall -Wextra -Werror)
        elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
            target_compile_options(${_apex_tgt} PRIVATE /WX)
        endif ()
    endforeach()
endif ()
