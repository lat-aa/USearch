# This CMake file is heavily inspired by following `usearch` CMake:
# https://github.com/nlohmann/json/blob/develop/CMakeLists.txt
cmake_minimum_required(VERSION 3.11 FATAL_ERROR)
project(
    usearch
    VERSION 2.26.2
    LANGUAGES CXX
    DESCRIPTION "Smaller & Faster Single-File Vector Search Engine from Unum"
    HOMEPAGE_URL "https://github.com/unum-cloud/USearch"
)

# Determine if USearch is built as a subproject (using `add_subdirectory`) or if it is the main project
set(USEARCH_IS_MAIN_PROJECT OFF)

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(USEARCH_IS_MAIN_PROJECT ON)
endif ()

# Allow CMake 3.13+ to override options when using FetchContent / add_subdirectory
if (POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif ()

# Options
option(USEARCH_INSTALL "Install CMake targets" OFF)

option(USEARCH_USE_OPENMP "Use OpenMP for a thread pool" OFF)
# 主工程默认开 NumKong（产品路径）；子工程 / 消毒矩阵可显式 -D USEARCH_USE_NUMKONG=0。
option(USEARCH_USE_NUMKONG "Use NumKong hardware-accelerated metrics" ${USEARCH_IS_MAIN_PROJECT})
option(USEARCH_USE_JEMALLOC "Use JeMalloc for faster memory allocations" OFF)

option(USEARCH_BUILD_TEST_CPP "Compile a native unit test in C++" ${USEARCH_IS_MAIN_PROJECT})
option(USEARCH_BUILD_BENCH_CPP "Compile a native benchmark in C++" ${USEARCH_IS_MAIN_PROJECT})
option(USEARCH_BUILD_API "Compile the C++ HTTP/MCP api binary" OFF)
# C 绑定（c/usearch.h + c/lib.cpp）；主工程默认开，供 FFI / test_capi
option(USEARCH_BUILD_C_API "Build C API static library" ${USEARCH_IS_MAIN_PROJECT})

# apex 网关/测试 -Werror：仅覆盖自有源码（api + tests/apex），不覆盖 vendored（llama/httplib/...）。
option(USEARCH_APEX_WERROR "Treat apex gateway/test warnings as errors (self-owned sources only)" OFF)

# Includes
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})
include(ExternalProject)
include(sanitizers)

# Configuration
include(GNUInstallDirs)
set(USEARCH_TARGET_NAME ${PROJECT_NAME})
set(USEARCH_CONFIG_INSTALL_DIR
    "${CMAKE_INSTALL_DATADIR}/cmake/${PROJECT_NAME}"
    CACHE INTERNAL ""
)
set(USEARCH_INCLUDE_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}")
set(USEARCH_TARGETS_EXPORT_NAME "${PROJECT_NAME}Targets")
set(USEARCH_CMAKE_CONFIG_TEMPLATE "cmake/config.cmake.in")
set(USEARCH_CMAKE_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(USEARCH_CMAKE_VERSION_CONFIG_FILE "${USEARCH_CMAKE_CONFIG_DIR}/${PROJECT_NAME}ConfigVersion.cmake")
set(USEARCH_CMAKE_PROJECT_CONFIG_FILE "${USEARCH_CMAKE_CONFIG_DIR}/${PROJECT_NAME}Config.cmake")
set(USEARCH_CMAKE_PROJECT_TARGETS_FILE "${USEARCH_CMAKE_CONFIG_DIR}/${PROJECT_NAME}Targets.cmake")
set(USEARCH_PKGCONFIG_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/pkgconfig")

include(CheckCXXSourceCompiles)
# Check for `__fp16` support
check_cxx_source_compiles(
    [=[
int main()
{
  __fp16 a = 1.0;
  __fp16 b = a + a;
  return 0;
}
]=]
    USEARCH_CAN_COMPILE_FP16
)

# Check for `_Float16` support
check_cxx_source_compiles(
    [=[
int main()
{
  _Float16 a = 1.0;
  _Float16 b = a + a;
  return 0;
}
]=]
    USEARCH_CAN_COMPILE_FLOAT16
)

# Check for `__bf16` support
check_cxx_source_compiles(
    [=[
int main()
{
  __bf16 a = 1.0;
  __bf16 b = a + a;
  return 0;
}
]=]
    USEARCH_CAN_COMPILE_BF16
)

# Check for `_Bfloat16` support
check_cxx_source_compiles(
    [=[
int main()
{
  _Bfloat16 a = 1.0;
  _Bfloat16 b = a + a;
  return 0;
}
]=]
    USEARCH_CAN_COMPILE_BFLOAT16
)

# Define our header-only library
add_library(${USEARCH_TARGET_NAME} INTERFACE)
add_library(${PROJECT_NAME}::${USEARCH_TARGET_NAME} ALIAS ${USEARCH_TARGET_NAME})
set(USEARCH_INCLUDE_BUILD_DIR "${PROJECT_SOURCE_DIR}/include/")

if (${CMAKE_VERSION} VERSION_LESS "3.8.0")
    target_compile_features(${USEARCH_TARGET_NAME} INTERFACE cxx_range_for)
else ()
    target_compile_features(${USEARCH_TARGET_NAME} INTERFACE cxx_std_11)
endif ()

# Core compilation settings affecting "index.hpp"
target_compile_definitions(${USEARCH_TARGET_NAME} INTERFACE "USEARCH_USE_OPENMP=$<BOOL:${USEARCH_USE_OPENMP}>")

# Supplementary compilation settings affecting "plugins/plugins.hpp"
target_compile_definitions(${USEARCH_TARGET_NAME} INTERFACE "USEARCH_USE_NUMKONG=$<BOOL:${USEARCH_USE_NUMKONG}>")

# Define which types can be compiled
target_compile_definitions(
    ${USEARCH_TARGET_NAME} INTERFACE "USEARCH_CAN_COMPILE_FP16=$<BOOL:${USEARCH_CAN_COMPILE_FP16}>"
)
target_compile_definitions(
    ${USEARCH_TARGET_NAME} INTERFACE "USEARCH_CAN_COMPILE_FLOAT16=$<BOOL:${USEARCH_CAN_COMPILE_FLOAT16}>"
)
target_compile_definitions(
    ${USEARCH_TARGET_NAME} INTERFACE "USEARCH_CAN_COMPILE_BF16=$<BOOL:${USEARCH_CAN_COMPILE_BF16}>"
)
target_compile_definitions(
    ${USEARCH_TARGET_NAME} INTERFACE "USEARCH_CAN_COMPILE_BFLOAT16=$<BOOL:${USEARCH_CAN_COMPILE_BFLOAT16}>"
)

target_include_directories(
    ${USEARCH_TARGET_NAME} ${USEARCH_SYSTEM_INCLUDE} INTERFACE $<BUILD_INTERFACE:${USEARCH_INCLUDE_BUILD_DIR}>
                                                               $<INSTALL_INTERFACE:include>
)
if (USEARCH_USE_NUMKONG)
    target_include_directories(
        ${USEARCH_TARGET_NAME} ${USEARCH_SYSTEM_INCLUDE}
        INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/numkong/include> $<INSTALL_INTERFACE:numkong/include>
    )
endif ()

# 分层 INTERFACE：只需版本宏时链 usearch_setup；完整索引用 usearch / plugins / dense。
add_library(usearch_setup INTERFACE)
add_library(${PROJECT_NAME}::usearch_setup ALIAS usearch_setup)
target_include_directories(
    usearch_setup ${USEARCH_SYSTEM_INCLUDE} INTERFACE $<BUILD_INTERFACE:${USEARCH_INCLUDE_BUILD_DIR}>
                                                      $<INSTALL_INTERFACE:include>
)

add_library(plugins INTERFACE)
add_library(${PROJECT_NAME}::plugins ALIAS plugins)
target_link_libraries(plugins INTERFACE ${USEARCH_TARGET_NAME})

add_library(dense INTERFACE)
add_library(${PROJECT_NAME}::dense ALIAS dense)
target_link_libraries(dense INTERFACE plugins)

# Install a pkg-config file, so other tools can find this
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/pkg-config.pc.in" "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pc")

# Install a custom package version config file instead of write_basic_package_version_file to ensure that it's
# architecture-independent https://github.com/nlohmann/json/issues/1697
include(CMakePackageConfigHelpers)
configure_file("cmake/usearchConfigVersion.cmake.in" ${USEARCH_CMAKE_VERSION_CONFIG_FILE} @ONLY)
configure_file(${USEARCH_CMAKE_CONFIG_TEMPLATE} ${USEARCH_CMAKE_PROJECT_CONFIG_FILE} @ONLY)

if (USEARCH_INSTALL)
    install(DIRECTORY ${USEARCH_INCLUDE_BUILD_DIR} DESTINATION ${USEARCH_INCLUDE_INSTALL_DIR})
    install(FILES ${USEARCH_CMAKE_PROJECT_CONFIG_FILE} ${USEARCH_CMAKE_VERSION_CONFIG_FILE}
            DESTINATION ${USEARCH_CONFIG_INSTALL_DIR}
    )
    export(
        TARGETS ${USEARCH_TARGET_NAME}
        NAMESPACE ${PROJECT_NAME}::
        FILE ${USEARCH_CMAKE_PROJECT_TARGETS_FILE}
    )
    install(
        TARGETS ${USEARCH_TARGET_NAME}
        EXPORT ${USEARCH_TARGETS_EXPORT_NAME}
        INCLUDES
        DESTINATION ${USEARCH_INCLUDE_INSTALL_DIR}
    )
    install(
        EXPORT ${USEARCH_TARGETS_EXPORT_NAME}
        NAMESPACE ${PROJECT_NAME}::
        DESTINATION ${USEARCH_CONFIG_INSTALL_DIR}
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pc" DESTINATION ${USEARCH_PKGCONFIG_INSTALL_DIR})
endif ()

# Find required packages
if (CMAKE_HOST_SYSTEM_NAME MATCHES "Darwin")
    set(CMAKE_THREAD_LIBS_INIT "-lpthread")
    set(CMAKE_HAVE_THREADS_LIBRARY 1)
    set(CMAKE_USE_WIN32_THREADS_INIT 0)
    set(CMAKE_USE_PTHREADS_INIT 1)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
endif ()

find_package(Threads REQUIRED)

if (USEARCH_USE_OPENMP)
    find_package(OpenMP REQUIRED)
endif ()

if (USEARCH_USE_JEMALLOC)
    include(jemalloc)
endif ()

# Set default build type to Release
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif ()

# Include directories for native targets (tests / benches)
set(USEARCH_HEADER_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/include")
if (USEARCH_USE_NUMKONG)
    list(APPEND USEARCH_HEADER_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/numkong/include")
endif ()

# 可移植核心选项：警告 / sanitizer / include / 定义；Release 用 -O2，不加 -march=native。
function (setup_core_target TARGET_NAME)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(
            ${TARGET_NAME}
            PRIVATE $<$<CONFIG:RELEASE>:-O2>
                    $<$<CONFIG:RELWITHDEBINFO>:-O2
                    -g>
                    $<$<CONFIG:DEBUG>:-O0
                    -g>
                    -fPIC
                    -Wall
                    -Wextra
                    -Wno-conversion
                    -Wno-unknown-pragmas
                    -fmax-errors=1
                    -pedantic
                    -fdiagnostics-color=always
        )
        target_link_options(${TARGET_NAME} PRIVATE -fPIC)
        if (USEARCH_USE_OPENMP)
            target_link_libraries(${TARGET_NAME} PRIVATE OpenMP::OpenMP_CXX)
        endif ()
        target_link_libraries(${TARGET_NAME} PRIVATE Threads::Threads)

    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(
            ${TARGET_NAME}
            PRIVATE $<$<CONFIG:RELEASE>:-O2>
                    $<$<CONFIG:RELWITHDEBINFO>:-O2
                    -g>
                    $<$<CONFIG:DEBUG>:-O0
                    -g>
                    -fPIC
                    -pedantic
                    -Wfatal-errors
                    -fcolor-diagnostics
        )
        target_link_options(${TARGET_NAME} PRIVATE -fPIC)
        if (USEARCH_USE_OPENMP)
            target_link_libraries(${TARGET_NAME} PRIVATE OpenMP::OpenMP_CXX)
        endif ()
        if (CMAKE_HOST_SYSTEM_NAME MATCHES "Darwin")
            target_link_libraries(
                ${TARGET_NAME} PRIVATE "-framework CoreFoundation" "-framework Security" Threads::Threads
            )
        else ()
            target_link_libraries(${TARGET_NAME} PRIVATE Threads::Threads)
        endif ()

    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "NVIDIA" OR CMAKE_CXX_COMPILER_ID STREQUAL "NVHPC")
        target_compile_options(${TARGET_NAME} PRIVATE $<$<CONFIG:RELEASE>:-O2>)
        target_compile_definitions(${TARGET_NAME} PRIVATE --expt-relaxed-constexpr --extended-lambda)
        set_property(SOURCE bench.cpp PROPERTY LANGUAGE CUDA)
        set_target_properties(${TARGET_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        set_target_properties(${TARGET_NAME} PROPERTIES CUDA_ARCHITECTURES "86")

    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
        target_compile_options(${TARGET_NAME} PRIVATE -w -ferror-limit=1)

    elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        target_compile_options(${TARGET_NAME} PRIVATE /wd4068 /wd4996)
        target_compile_options(
            ${TARGET_NAME} PRIVATE $<$<CONFIG:DEBUG>:/DEBUG> $<$<CONFIG:RELEASE>:/O2> $<$<CONFIG:RELWITHDEBINFO>:/O2
                                   /DEBUG>
        )
    endif ()

    usearch_apply_sanitizers(${TARGET_NAME})
    target_include_directories(${TARGET_NAME} PRIVATE ${USEARCH_HEADER_INCLUDES})
    set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD 17)

    set_target_properties(
        ${TARGET_NAME}
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
                   ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
                   LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
                   RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
                   ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
                   LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
                   RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
                   ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
                   LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
                   RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
                   ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
                   LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
    )

    get_target_property(TARGET_TYPE ${TARGET_NAME} TYPE)
    if (TARGET_TYPE STREQUAL "SHARED_LIBRARY" OR TARGET_TYPE STREQUAL "STATIC_LIBRARY")
        set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "lib")
    endif ()

    target_compile_definitions(${TARGET_NAME} PRIVATE "USEARCH_USE_OPENMP=$<BOOL:${USEARCH_USE_OPENMP}>")
    target_compile_definitions(${TARGET_NAME} PRIVATE "USEARCH_USE_NUMKONG=$<BOOL:${USEARCH_USE_NUMKONG}>")

    if (USEARCH_USE_NUMKONG AND TARGET nk_shared)
        target_compile_definitions(${TARGET_NAME} PRIVATE "NK_DYNAMIC_DISPATCH=1")
        target_link_libraries(${TARGET_NAME} PRIVATE nk_shared)
        if (APPLE)
            set(_USEARCH_NUMKONG_RPATH "@loader_path;@loader_path/numkong")
        elseif (UNIX AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
            set(_USEARCH_NUMKONG_RPATH "$ORIGIN;$ORIGIN/numkong")
        endif ()
        if (DEFINED _USEARCH_NUMKONG_RPATH)
            set_target_properties(
                ${TARGET_NAME} PROPERTIES BUILD_RPATH "${_USEARCH_NUMKONG_RPATH}"
                                          INSTALL_RPATH "${_USEARCH_NUMKONG_RPATH}"
            )
        endif ()
    endif ()
endfunction ()

# 本机性能选项：在 core 上叠加 -O3 / -march=native / -ffast-math（bench 用；服务端勿用）。
function (setup_perf_target TARGET_NAME)
    setup_core_target(${TARGET_NAME})
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(
            ${TARGET_NAME}
            PRIVATE $<$<CONFIG:RELEASE>:-O3>
                    $<$<CONFIG:RELWITHDEBINFO>:-O3>
                    -ffast-math
                    -march=native
        )
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "NVIDIA" OR CMAKE_CXX_COMPILER_ID STREQUAL "NVHPC")
        target_compile_options(${TARGET_NAME} PRIVATE $<$<CONFIG:RELEASE>:-O3>)
    endif ()
endfunction ()

# 兼容旧调用：默认走性能档（与历史 bench/test 行为接近）。
function (setup_target TARGET_NAME)
    setup_perf_target(${TARGET_NAME})
endfunction ()

# Delegate NumKong compilation to its own CMakeLists.txt — it handles ISA detection, NK_TARGET_* flags, and SIMD
# dispatch backends internally.
if (USEARCH_USE_NUMKONG)
    set(NK_BUILD_SHARED
        ON
        CACHE BOOL "" FORCE
    )
    add_subdirectory(numkong)

    if (TARGET nk_shared)
        set_target_properties(nk_shared PROPERTIES ENABLE_EXPORTS ON)

        # Propagate NumKong to in-tree consumers (BUILD_INTERFACE) so link/includes/defs travel together.
        # Avoid exporting nk_shared into the installed USearch package (submodule is not in the export set).
        target_compile_definitions(${USEARCH_TARGET_NAME} INTERFACE
                                   $<BUILD_INTERFACE:NK_DYNAMIC_DISPATCH=1>)
        target_link_libraries(${USEARCH_TARGET_NAME} INTERFACE $<BUILD_INTERFACE:nk_shared>)

        # Windows has no RPATH equivalent, so the dynamic loader only finds
        # NumKong reliably when it sits next to the binaries that link it.
        if (WIN32)
            set_target_properties(
                nk_shared
                PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
                           RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
                           RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
                           RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
                           RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}"
            )
        endif ()
    endif ()
endif ()

# Must be called before "add_subdirectory()". See
# https://stackoverflow.com/questions/30250494/ctest-not-detecting-tests.
enable_testing()

# C API：静态库，头文件在 c/；与 header-only 核心并列。
if (USEARCH_BUILD_C_API)
    add_library(usearch_c STATIC c/lib.cpp)
    setup_core_target(usearch_c)
    target_include_directories(
        usearch_c
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/c>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${USEARCH_INCLUDE_INSTALL_DIR}>
    )
    target_link_libraries(usearch_c PUBLIC ${USEARCH_TARGET_NAME})
endif ()

# cpp/：bench + fuzz；tools/apex/：HTTP/MCP 网关（与核心解耦）。
if (${USEARCH_BUILD_TEST_CPP} OR ${USEARCH_BUILD_BENCH_CPP} OR ${USEARCH_BUILD_FUZZ})
    add_subdirectory(cpp)
endif ()

if (USEARCH_BUILD_API)
    add_subdirectory(tools/apex)
endif ()

# 单元测试源码只放 tests/（与 cpp/ 产品代码分离）。
if (USEARCH_BUILD_TEST_CPP)
    add_subdirectory(tests)
endif ()
