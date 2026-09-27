/**
 *  @file       kinds.hpp
 *  @brief      标量/度量枚举、uuid、别名与名解析；插件栈根依赖。
 */
#pragma once
#define __STDC_WANT_IEC_60559_TYPES_EXT__
#include <float.h>  // `_Float16`
#include <stdlib.h> // `aligned_alloc`

#include <atomic>  // `std::atomic`
#include <chrono>  // `std::chrono`
#include <cstring> // `std::strncmp`
#include <thread>  // `std::thread`

// 只要 util/heap（expected_gt、uint40、byte_t）；勿拉 hnsw，缩短仅用度量插件的编译。
#include <usearch/heap.hpp>

#if defined(USEARCH_DEFINED_LINUX)
#include <sys/auxv.h> // `getauxval()`
#endif

#if !defined(USEARCH_USE_OPENMP)
#define USEARCH_USE_OPENMP 0
#endif

#if USEARCH_USE_OPENMP
#include <omp.h> // `omp_get_num_threads()`
#endif

#if !defined(USEARCH_USE_NUMKONG)
#define USEARCH_USE_NUMKONG 0
#endif

#if USEARCH_USE_NUMKONG
// Propagate the `f16` settings
#if defined(USEARCH_CAN_COMPILE_FP16) || defined(USEARCH_CAN_COMPILE_FLOAT16)
#if USEARCH_CAN_COMPILE_FP16 || USEARCH_CAN_COMPILE_FLOAT16
#define NK_NATIVE_F16 1
#else
#define NK_NATIVE_F16 0
#endif
#endif
// Propagate the `bf16` settings
#if defined(USEARCH_CAN_COMPILE_BF16) || defined(USEARCH_CAN_COMPILE_BFLOAT16)
#if USEARCH_CAN_COMPILE_BF16 || USEARCH_CAN_COMPILE_BFLOAT16
#define NK_NATIVE_BF16 1
#else
#define NK_NATIVE_BF16 0
#endif
#endif
// No problem, if some of the functions are unused or undefined
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wunused"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4101) // "Unused variables"
#pragma warning(disable : 4068) // "Unknown pragmas", when MSVC tries to read GCC pragmas
#endif                          // _MSC_VER
#include <numkong/numkong.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif // _MSC_VER
#pragma GCC diagnostic pop
#endif
namespace unum {
namespace usearch {

using u40_t = uint40_t;
enum b1x8_t : unsigned char {};

struct uuid_t {
    std::uint8_t octets[16];
};

class bf16_bits_t;
class f16_bits_t;
class e5m2_bits_t;
class e4m3_bits_t;
class e3m2_bits_t;
class e2m3_bits_t;

using bf16_t = bf16_bits_t;
using f16_t = f16_bits_t;
using e5m2_t = e5m2_bits_t;
using e4m3_t = e4m3_bits_t;
using e3m2_t = e3m2_bits_t;
using e2m3_t = e2m3_bits_t;

using f64_t = double;
using f32_t = float;

using u64_t = std::uint64_t;
using u32_t = std::uint32_t;
using u16_t = std::uint16_t;
using u8_t = std::uint8_t;

using i64_t = std::int64_t;
using i32_t = std::int32_t;
using i16_t = std::int16_t;
using i8_t = std::int8_t;

/**
 *  @brief  Reinterpret-cast between float and uint32 without UB on most compilers.
 */
union fu32_t {
    float f;
    std::uint32_t u;
};

/**
 *  @brief  Enumerates the most commonly used distance metrics, mostly for dense vector representations.
 */
enum class metric_kind_t : std::uint8_t {
    unknown_k = 0,
    // Classics:
    ip_k = 'i',
    cos_k = 'c',
    l2sq_k = 'e',

    // Custom:
    pearson_k = 'p',
    haversine_k = 'h',
    divergence_k = 'd',

    // Dense Sets:
    hamming_k = 'b',
    tanimoto_k = 't',
    sorensen_k = 's',

    // Sparse Sets:
    jaccard_k = 'j',
};

/**
 *  @brief  Enumerates the most commonly used scalar types, mostly for dense vector representations.
 *          Doesn't include logical types, like complex numbers or quaternions.
 */
enum class scalar_kind_t : std::uint8_t {
    unknown_k = 0,
    // Custom:
    b1x8_k = 1,
    u40_k = 2,
    uuid_k = 3,
    bf16_k = 4,
    // Mini-floats:
    e5m2_k = 5, ///< FP8 IEEE 754: 1 sign + 5 exponent + 2 mantissa, range +/-57344
    e4m3_k = 6, ///< FP8 OCP: 1 sign + 4 exponent + 3 mantissa, range +/-448
    e3m2_k = 8, ///< FP6: 1 sign + 3 exponent + 2 mantissa, range +/-28
    e2m3_k = 7, ///< FP6: 1 sign + 2 exponent + 3 mantissa, range +/-7.5
    // Common:
    f64_k = 10,
    f32_k = 11,
    f16_k = 12,
    // Common Integral:
    u64_k = 14,
    u32_k = 15,
    u16_k = 16,
    u8_k = 17,
    i64_k = 20,
    i32_k = 21,
    i16_k = 22,
    i8_k = 23,
};

/**
 *  @brief  Maps a scalar type to its corresponding scalar_kind_t enumeration value.
 */
template <typename scalar_at> scalar_kind_t scalar_kind() noexcept {
    if (std::is_same<scalar_at, b1x8_t>())
        return scalar_kind_t::b1x8_k;
    if (std::is_same<scalar_at, uint40_t>())
        return scalar_kind_t::u40_k;
    if (std::is_same<scalar_at, uuid_t>())
        return scalar_kind_t::uuid_k;
    if (std::is_same<scalar_at, f64_t>())
        return scalar_kind_t::f64_k;
    if (std::is_same<scalar_at, f32_t>())
        return scalar_kind_t::f32_k;
    if (std::is_same<scalar_at, f16_t>())
        return scalar_kind_t::f16_k;
    if (std::is_same<scalar_at, bf16_t>())
        return scalar_kind_t::bf16_k;
    if (std::is_same<scalar_at, e5m2_t>())
        return scalar_kind_t::e5m2_k;
    if (std::is_same<scalar_at, e4m3_t>())
        return scalar_kind_t::e4m3_k;
    if (std::is_same<scalar_at, e3m2_t>())
        return scalar_kind_t::e3m2_k;
    if (std::is_same<scalar_at, e2m3_t>())
        return scalar_kind_t::e2m3_k;
    if (std::is_same<scalar_at, i8_t>())
        return scalar_kind_t::i8_k;
    if (std::is_same<scalar_at, u64_t>())
        return scalar_kind_t::u64_k;
    if (std::is_same<scalar_at, u32_t>())
        return scalar_kind_t::u32_k;
    if (std::is_same<scalar_at, u16_t>())
        return scalar_kind_t::u16_k;
    if (std::is_same<scalar_at, u8_t>())
        return scalar_kind_t::u8_k;
    if (std::is_same<scalar_at, i64_t>())
        return scalar_kind_t::i64_k;
    if (std::is_same<scalar_at, i32_t>())
        return scalar_kind_t::i32_k;
    if (std::is_same<scalar_at, i16_t>())
        return scalar_kind_t::i16_k;
    if (std::is_same<scalar_at, i8_t>())
        return scalar_kind_t::i8_k;
    return scalar_kind_t::unknown_k;
}

/**
 *  @brief  Converts an angle from degrees to radians.
 */
template <typename at> at angle_to_radians(at angle) noexcept { return angle * at(3.14159265358979323846) / at(180); }

/**
 *  @brief  Readability helper to compute the square of a given value.
 */
template <typename at> at square(at value) noexcept { return value * value; }

/**
 *  @brief  Clamps a value between a lower and upper bound using a custom comparator. Similar to `std::clamp`.
 *          https://en.cppreference.com/w/cpp/algorithm/clamp
 */
template <typename at, typename compare_at> inline at clamp(at v, at lo, at hi, compare_at comp) noexcept {
    return comp(v, lo) ? lo : comp(hi, v) ? hi : v;
}

/**
 *  @brief  Clamps a value between a lower and upper bound. Similar to `std::clamp`.
 *          https://en.cppreference.com/w/cpp/algorithm/clamp
 */
template <typename at> inline at clamp(at v, at lo, at hi) noexcept {
    return usearch::clamp(v, lo, hi, std::less<at>{});
}

/**
 *  @brief  Compares two strings for equality, given a length for the first string.
 */
inline bool str_equals(char const* first_begin, std::size_t first_len, char const* second_begin) noexcept {
    std::size_t second_len = std::strlen(second_begin);
    return first_len == second_len && std::strncmp(first_begin, second_begin, first_len) == 0;
}

/**
 *  @brief  Returns the number of bits required to represent a scalar type.
 */
inline std::size_t bits_per_scalar(scalar_kind_t scalar_kind) noexcept {
    switch (scalar_kind) {
    case scalar_kind_t::uuid_k: return 128;
    case scalar_kind_t::u40_k: return 40;
    case scalar_kind_t::bf16_k: return 16;
    case scalar_kind_t::b1x8_k: return 1;
    case scalar_kind_t::u64_k: return 64;
    case scalar_kind_t::i64_k: return 64;
    case scalar_kind_t::f64_k: return 64;
    case scalar_kind_t::u32_k: return 32;
    case scalar_kind_t::i32_k: return 32;
    case scalar_kind_t::f32_k: return 32;
    case scalar_kind_t::u16_k: return 16;
    case scalar_kind_t::i16_k: return 16;
    case scalar_kind_t::f16_k: return 16;
    case scalar_kind_t::u8_k: return 8;
    case scalar_kind_t::i8_k: return 8;
    case scalar_kind_t::e5m2_k: return 8;
    case scalar_kind_t::e4m3_k: return 8;
    case scalar_kind_t::e2m3_k: return 8;
    case scalar_kind_t::e3m2_k: return 8;
    default: return 0;
    }
}

/**
 *  @brief  Returns the number of bits in a scalar word for a given scalar type.
 *          Equivalent to `bits_per_scalar` for types that are not bit-packed.
 */
inline std::size_t bits_per_scalar_word(scalar_kind_t scalar_kind) noexcept {
    switch (scalar_kind) {
    case scalar_kind_t::uuid_k: return 128;
    case scalar_kind_t::u40_k: return 40;
    case scalar_kind_t::bf16_k: return 16;
    case scalar_kind_t::b1x8_k: return 8;
    case scalar_kind_t::u64_k: return 64;
    case scalar_kind_t::i64_k: return 64;
    case scalar_kind_t::f64_k: return 64;
    case scalar_kind_t::u32_k: return 32;
    case scalar_kind_t::i32_k: return 32;
    case scalar_kind_t::f32_k: return 32;
    case scalar_kind_t::u16_k: return 16;
    case scalar_kind_t::i16_k: return 16;
    case scalar_kind_t::f16_k: return 16;
    case scalar_kind_t::u8_k: return 8;
    case scalar_kind_t::i8_k: return 8;
    case scalar_kind_t::e5m2_k: return 8;
    case scalar_kind_t::e4m3_k: return 8;
    case scalar_kind_t::e2m3_k: return 8;
    case scalar_kind_t::e3m2_k: return 8;
    default: return 0;
    }
}

/**
 *  @brief  Returns the string name of a given scalar type.
 */
inline char const* scalar_kind_name(scalar_kind_t scalar_kind) noexcept {
    switch (scalar_kind) {
    case scalar_kind_t::uuid_k: return "uuid";
    case scalar_kind_t::u40_k: return "u40";
    case scalar_kind_t::bf16_k: return "bf16";
    case scalar_kind_t::b1x8_k: return "b1x8";
    case scalar_kind_t::u64_k: return "u64";
    case scalar_kind_t::i64_k: return "i64";
    case scalar_kind_t::f64_k: return "f64";
    case scalar_kind_t::u32_k: return "u32";
    case scalar_kind_t::i32_k: return "i32";
    case scalar_kind_t::f32_k: return "f32";
    case scalar_kind_t::u16_k: return "u16";
    case scalar_kind_t::i16_k: return "i16";
    case scalar_kind_t::f16_k: return "f16";
    case scalar_kind_t::u8_k: return "u8";
    case scalar_kind_t::i8_k: return "i8";
    case scalar_kind_t::e5m2_k: return "e5m2";
    case scalar_kind_t::e4m3_k: return "e4m3";
    case scalar_kind_t::e2m3_k: return "e2m3";
    case scalar_kind_t::e3m2_k: return "e3m2";
    default: return "";
    }
}

/**
 *  @brief  Returns the string name of a given distance metric.
 */
inline char const* metric_kind_name(metric_kind_t metric) noexcept {
    switch (metric) {
    case metric_kind_t::unknown_k: return "unknown";
    case metric_kind_t::ip_k: return "ip";
    case metric_kind_t::cos_k: return "cos";
    case metric_kind_t::l2sq_k: return "l2sq";
    case metric_kind_t::pearson_k: return "pearson";
    case metric_kind_t::haversine_k: return "haversine";
    case metric_kind_t::divergence_k: return "divergence";
    case metric_kind_t::jaccard_k: return "jaccard";
    case metric_kind_t::hamming_k: return "hamming";
    case metric_kind_t::tanimoto_k: return "tanimoto";
    case metric_kind_t::sorensen_k: return "sorensen";
    default: return "";
    }
}

/**
 *  @brief  Parses a string to identify the corresponding `scalar_kind_t` enumeration value.
 */
inline expected_gt<scalar_kind_t> scalar_kind_from_name(char const* name, std::size_t len) {
    expected_gt<scalar_kind_t> parsed;
    if (str_equals(name, len, "f64"))
        parsed.result = scalar_kind_t::f64_k;
    else if (str_equals(name, len, "f32"))
        parsed.result = scalar_kind_t::f32_k;
    else if (str_equals(name, len, "bf16"))
        parsed.result = scalar_kind_t::bf16_k;
    else if (str_equals(name, len, "f16"))
        parsed.result = scalar_kind_t::f16_k;
    else if (str_equals(name, len, "e5m2"))
        parsed.result = scalar_kind_t::e5m2_k;
    else if (str_equals(name, len, "e4m3"))
        parsed.result = scalar_kind_t::e4m3_k;
    else if (str_equals(name, len, "e3m2"))
        parsed.result = scalar_kind_t::e3m2_k;
    else if (str_equals(name, len, "e2m3"))
        parsed.result = scalar_kind_t::e2m3_k;
    else if (str_equals(name, len, "i8"))
        parsed.result = scalar_kind_t::i8_k;
    else if (str_equals(name, len, "u8"))
        parsed.result = scalar_kind_t::u8_k;
    else if (str_equals(name, len, "b1"))
        parsed.result = scalar_kind_t::b1x8_k;
    else
        parsed.failed("Unknown type, choose: f64, f32, bf16, f16, e5m2, e4m3, e3m2, e2m3, i8, u8, b1");
    return parsed;
}

/**
 *  @brief  Parses a string to identify the corresponding `scalar_kind_t` enumeration value.
 */
inline expected_gt<scalar_kind_t> scalar_kind_from_name(char const* name) {
    return scalar_kind_from_name(name, std::strlen(name));
}

/**
 *  @brief  Parses a string to identify the corresponding `metric_kind_t` enumeration value.
 */
inline expected_gt<metric_kind_t> metric_from_name(char const* name, std::size_t len) {
    expected_gt<metric_kind_t> parsed;
    if (str_equals(name, len, "l2sq") || str_equals(name, len, "euclidean_sq")) {
        parsed.result = metric_kind_t::l2sq_k;
    } else if (str_equals(name, len, "ip") || str_equals(name, len, "inner") || str_equals(name, len, "dot")) {
        parsed.result = metric_kind_t::ip_k;
    } else if (str_equals(name, len, "cos") || str_equals(name, len, "angular")) {
        parsed.result = metric_kind_t::cos_k;
    } else if (str_equals(name, len, "haversine")) {
        parsed.result = metric_kind_t::haversine_k;
    } else if (str_equals(name, len, "divergence")) {
        parsed.result = metric_kind_t::divergence_k;
    } else if (str_equals(name, len, "pearson")) {
        parsed.result = metric_kind_t::pearson_k;
    } else if (str_equals(name, len, "hamming")) {
        parsed.result = metric_kind_t::hamming_k;
    } else if (str_equals(name, len, "tanimoto")) {
        parsed.result = metric_kind_t::tanimoto_k;
    } else if (str_equals(name, len, "sorensen")) {
        parsed.result = metric_kind_t::sorensen_k;
    } else
        parsed.failed("Unknown distance, choose: l2sq, ip, cos, haversine, divergence, jaccard, pearson, hamming, "
                      "tanimoto, sorensen");
    return parsed;
}

/**
 *  @brief  Parses a string to identify the corresponding `metric_kind_t` enumeration value.
 */
inline expected_gt<metric_kind_t> metric_from_name(char const* name) {
    return metric_from_name(name, std::strlen(name));
}

} // namespace usearch
} // namespace unum
