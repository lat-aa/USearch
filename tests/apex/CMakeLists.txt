# apex 纯函数 / gate 桩测：helpers+turn 不依赖 llama；gate 链 stub 替 Encoder/Store。
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

add_executable(apexverdict verdict.cpp)
target_compile_features(apexverdict PRIVATE cxx_std_17)
add_test(NAME apexverdict COMMAND apexverdict)
set_tests_properties(apexverdict PROPERTIES LABELS "apex;unit" TIMEOUT 30)

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

# gate 集成：gate.cpp+decide+text+stub；api.hpp 需 httplib 头，但不链 llama
if (NOT TARGET tomlplusplus::tomlplusplus)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/toml.cmake)
endif ()
if (NOT TARGET httplib::httplib)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/httplib.cmake)
endif ()

set(APEX_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/apex)
add_executable(apexgate
    gate.cpp
    stub.cpp
    ${APEX_SRC}/gate.cpp
    ${APEX_SRC}/decide.cpp
    ${APEX_SRC}/text.cpp
)
target_compile_features(apexgate PRIVATE cxx_std_17)
target_include_directories(apexgate PRIVATE
    ${APEX_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
target_link_libraries(apexgate PRIVATE
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus
    httplib::httplib
)
if (WIN32)
    target_compile_definitions(apexgate PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
endif ()
add_test(NAME apexgate COMMAND apexgate)
set_tests_properties(apexgate PROPERTIES LABELS "apex;unit" TIMEOUT 60)
