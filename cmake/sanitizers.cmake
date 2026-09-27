# Sanitizer / coverage helpers for USearch native targets.
# Prefer explicit -DUSEARCH_ENABLE_*=ON in CI; Debug still gets ASan+UBSan by default.

option(USEARCH_ENABLE_ASAN "Enable AddressSanitizer for native targets" OFF)
option(USEARCH_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for native targets" OFF)
option(USEARCH_ENABLE_TSAN "Enable ThreadSanitizer (mutually exclusive with ASan)" OFF)
option(USEARCH_ENABLE_COVERAGE "Enable coverage instrumentation (GCC/Clang)" OFF)
option(USEARCH_BUILD_FUZZ "Build libFuzzer target fuzz_index" OFF)
option(USEARCH_SANITIZE_DEBUG "If no explicit sanitizer is ON, keep ASan+UBSan on Debug configs" ON)

if (USEARCH_ENABLE_TSAN AND USEARCH_ENABLE_ASAN)
    message(FATAL_ERROR "USEARCH_ENABLE_TSAN and USEARCH_ENABLE_ASAN cannot be enabled together")
endif ()

function (usearch_apply_sanitizers TARGET_NAME)
    if (NOT TARGET ${TARGET_NAME})
        return()
    endif ()
    # libFuzzer 目标自带 -fsanitize=fuzzer,...，避免与通用消毒重复链接。
    if (TARGET_NAME STREQUAL "fuzz_index")
        return()
    endif ()

    if (USEARCH_ENABLE_COVERAGE AND (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${TARGET_NAME} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
            target_link_options(${TARGET_NAME} PRIVATE -fprofile-instr-generate)
        else ()
            target_compile_options(${TARGET_NAME} PRIVATE --coverage)
            target_link_options(${TARGET_NAME} PRIVATE --coverage)
        endif ()
    endif ()

    set(_explicit OFF)
    if (USEARCH_ENABLE_ASAN OR USEARCH_ENABLE_UBSAN OR USEARCH_ENABLE_TSAN)
        set(_explicit ON)
    endif ()

    if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        if (USEARCH_ENABLE_ASAN)
            target_compile_options(${TARGET_NAME} PRIVATE /fsanitize=address /Zi)
            target_link_options(${TARGET_NAME} PRIVATE /INCREMENTAL:NO)
        endif ()
        return()
    endif ()

    if (NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        return()
    endif ()

    if (USEARCH_ENABLE_TSAN)
        target_compile_options(${TARGET_NAME} PRIVATE -g -O1 -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${TARGET_NAME} PRIVATE -fsanitize=thread)
        return()
    endif ()

    if (_explicit)
        set(_flags "")
        if (USEARCH_ENABLE_ASAN)
            list(APPEND _flags -fsanitize=address -fsanitize=alignment)
            if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT APPLE))
                list(APPEND _flags -fsanitize=leak)
            endif ()
        endif ()
        if (USEARCH_ENABLE_UBSAN)
            list(APPEND _flags -fsanitize=undefined)
        endif ()
        target_compile_options(${TARGET_NAME} PRIVATE -g -O1 -fno-omit-frame-pointer ${_flags})
        target_link_options(${TARGET_NAME} PRIVATE ${_flags})
        return()
    endif ()

    if (USEARCH_SANITIZE_DEBUG)
        target_compile_options(
            ${TARGET_NAME}
            PRIVATE $<$<CONFIG:DEBUG>:-O0
                    -g
                    -fsanitize=address
                    -fsanitize=alignment
                    -fsanitize=undefined
                    -fno-omit-frame-pointer>
        )
        target_link_options(
            ${TARGET_NAME}
            PRIVATE
            $<$<CONFIG:DEBUG>:-g
            -fsanitize=address
            -fsanitize=alignment
            -fsanitize=undefined>
        )
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT APPLE))
            target_compile_options(${TARGET_NAME} PRIVATE $<$<CONFIG:DEBUG>:-fsanitize=leak>)
            target_link_options(${TARGET_NAME} PRIVATE $<$<CONFIG:DEBUG>:-fsanitize=leak>)
        endif ()
    endif ()
endfunction ()
