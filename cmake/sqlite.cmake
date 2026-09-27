include(FetchContent)

# 官方 amalgamation：单文件 sqlite3.c/h，Windows 免系统依赖。
FetchContent_Declare(
    sqlite_amalgamation
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
)
FetchContent_GetProperties(sqlite_amalgamation)
if (NOT sqlite_amalgamation_POPULATED)
    FetchContent_Populate(sqlite_amalgamation)
endif ()

add_library(sqlite3 STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
target_compile_definitions(sqlite3 PUBLIC SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
if (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(sqlite3 PRIVATE -Wno-unused-parameter -Wno-cast-function-type)
endif ()
set_target_properties(sqlite3 PROPERTIES C_STANDARD 99)
