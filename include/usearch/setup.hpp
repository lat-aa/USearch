/**
 *  @file       setup.hpp
 *  @brief      版本号、平台/编译器宏与基础 STL/OS 头；不含索引算法。
 */
#pragma once
#define USEARCH_VERSION_MAJOR 2
#define USEARCH_VERSION_MINOR 26
#define USEARCH_VERSION_PATCH 2

// Inferring C++ version
// https://stackoverflow.com/a/61552074
#if ((defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L)
#define USEARCH_DEFINED_CPP17
#endif
#if ((defined(_MSVC_LANG) && _MSVC_LANG >= 202002L) || __cplusplus >= 202002L)
#define USEARCH_DEFINED_CPP20
#endif

// Inferring target OS: Windows, MacOS, or Linux
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#define USEARCH_DEFINED_WINDOWS
#elif defined(__APPLE__) && defined(__MACH__)
#define USEARCH_DEFINED_APPLE
#elif defined(__linux__)
#define USEARCH_DEFINED_LINUX
#if defined(__ANDROID_API__)
#define USEARCH_DEFINED_ANDROID
#endif
#endif

// Inferring the compiler: Clang vs GCC
#if defined(__clang__)
#define USEARCH_DEFINED_CLANG
#elif defined(__GNUC__)
#define USEARCH_DEFINED_GCC
#endif

// The `#pragma region` and `#pragma endregion` are not supported by GCC 12 and older.
// But they are supported by GCC 13, all recent Clang versions, and MSVC.
#if defined(__GNUC__) && ((__GNUC__ > 13) || (__GNUC__ == 13 && __GNUC_MINOR__ >= 0))
#define USEARCH_USE_PRAGMA_REGION
#elif defined(__clang__) || defined(_MSC_VER)
#define USEARCH_USE_PRAGMA_REGION
#endif

// Inferring hardware architecture: x86 vs Arm
#if defined(__x86_64__)
#define USEARCH_DEFINED_X86
#elif defined(__aarch64__)
#define USEARCH_DEFINED_ARM
#endif

// Inferring hardware bitness: 32 vs 64
// Using compiler predefined macros for is technically safer than including `<cstdint>` and
// using the commonly advised `UINTPTR_MAX` trick, as that constant is optional in standard C/C++.
// https://stackoverflow.com/a/5273354
// https://en.cppreference.com/w/cpp/types/integer.html
#if defined(_WIN64) || defined(__LP64__) || defined(__x86_64__) || defined(__aarch64__) || defined(__powerpc64__)
#define USEARCH_64BIT_ENV
#else
#define USEARCH_32BIT_ENV
#endif

#if !defined(USEARCH_USE_OPENMP)
#define USEARCH_USE_OPENMP 0
#endif

// OS-specific includes
#if defined(USEARCH_DEFINED_WINDOWS)
#define _USE_MATH_DEFINES
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <sys/stat.h> // `fstat` for file size
#undef NOMINMAX
#undef _USE_MATH_DEFINES
#else
#include <fcntl.h>    // `fallocate`
#include <stdlib.h>   // `posix_memalign`
#include <sys/mman.h> // `mmap`
#include <sys/stat.h> // `fstat` for file size
#include <unistd.h>   // `open`, `close`
#endif

// STL includes
#include <algorithm> // `std::sort_heap`
#include <atomic>    // `std::atomic`
#include <bitset>    // `std::bitset`
#include <climits>   // `CHAR_BIT`
#include <cmath>     // `std::sqrt`
#include <cstring>   // `std::memset`
#include <iterator>  // `std::reverse_iterator`
#include <mutex>     // `std::unique_lock` - replacement candidate
#include <random>    // `std::default_random_engine` - replacement candidate
#include <stdexcept> // `std::runtime_exception`
#include <thread>    // `std::thread`
#include <utility>   // `std::pair`

// Helper macros for concatenation and stringification
#define usearch_concat_helper_m(a, b) a##b
#define usearch_concat_m(a, b) usearch_concat_helper_m(a, b)
#define usearch_stringify_helper_m(x) #x
#define usearch_stringify_m(x) usearch_stringify_helper_m(x)

// Prefetching
#if defined(USEARCH_DEFINED_GCC)
// https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html
// Zero means we are only going to read from that memory.
// Three means high temporal locality and suggests to keep
// the data in all layers of cache.
#define usearch_prefetch_m(ptr) __builtin_prefetch((void*)(ptr), 0, 3)
#elif defined(USEARCH_DEFINED_X86)
#define usearch_prefetch_m(ptr) _mm_prefetch((void*)(ptr), _MM_HINT_T0)
#else
#define usearch_prefetch_m(ptr)
#endif

// Function profiling
#if defined(usearch_defined_x86)
#define usearch_profiled_m __attribute__((noinline))
#define usearch_profile_name_m(name)                                                                                   \
    __asm__ volatile(".globl " usearch_stringify_m(usearch_concat_m(name, __COUNTER__)) "\n" usearch_stringify_m(      \
        usearch_concat_m(name, __COUNTER__)) ":")
#elif defined(usearch_defined_arm)
#define usearch_profiled_m __attribute__((noinline))
#define usearch_profile_name_m(name)                                                                                   \
    __asm__ volatile(".global " usearch_stringify_m(usearch_concat_m(name, __COUNTER__)) "\n" usearch_stringify_m(     \
        usearch_concat_m(name, __COUNTER__)) ":")
#else
#define usearch_profiled_m
#define usearch_profile_name_m(name)
#endif

// Alignment
#if defined(USEARCH_DEFINED_WINDOWS)
#define usearch_pack_m
#define usearch_align_m __declspec(align(64))
#else
#define usearch_pack_m __attribute__((packed))
#define usearch_align_m __attribute__((aligned(64)))
#endif

// Debugging
#if defined(NDEBUG)
#define usearch_assert_m(must_be_true, message)
#define usearch_noexcept_m noexcept
#else
#define usearch_assert_m(must_be_true, message)                                                                        \
    if (!(must_be_true)) {                                                                                             \
        usearch_raise_runtime_error(message);                                                                          \
    }
#define usearch_noexcept_m
#endif

extern "C" {
/// @brief  Helper function to simplify debugging - trace just one symbol - `usearch_raise_runtime_error`.
///         Assuming the `extern C` block, the name won't be mangled.
inline static void usearch_raise_runtime_error(char const* message) {
    // On Windows we compile with `/EHc` flag, which specifies that functions
    // with C linkage do not throw C++ exceptions.
#if !defined(__cpp_exceptions) || defined(USEARCH_DEFINED_WINDOWS)
    std::terminate();
#else
    throw std::runtime_error(message);
#endif
}
}
