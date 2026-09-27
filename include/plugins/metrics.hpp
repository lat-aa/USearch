/**
 *  @file       metrics.hpp
 *  @brief      各 metric_*_gt 与 distance_punned 别名。
 */
#pragma once
#include <plugins/casts.hpp>
namespace unum {
namespace usearch {

/*  Don't complain if the vectorization of the inner loops fails:
 *
 *  > warning: loop not vectorized: the optimizer was unable to perform the requested transformation;
 *  > the transformation might be disabled or specified as part of an unsupported transformation ordering
 */
#if defined(USEARCH_DEFINED_CLANG)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpass-failed"
#endif

/**
 *  @brief  Inner (Dot) Product distance.
 *          Vectors should be normalized to unit length,
 *          otherwise `::metric_cos_gt` should be used instead.
 */
template <typename scalar_at = float, typename result_at = scalar_at> struct metric_ip_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t dim) const noexcept {
        result_t ab{};
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : ab)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != dim; ++i)
            ab += result_t(a[i]) * result_t(b[i]);
        return 1 - ab;
    }
};

/**
 *  @brief  Cosine (Angular) distance.
 *          Identical to the Inner Product of normalized vectors.
 *          Unless you are running on an tiny embedded platform, this metric
 *          is recommended over `::metric_ip_gt` for low-precision scalars.
 */
template <typename scalar_at = float, typename result_at = scalar_at> struct metric_cos_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t dim) const noexcept {
        result_t ab{}, a2{}, b2{};
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : ab, a2, b2)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != dim; ++i) {
            result_t ai = static_cast<result_t>(a[i]);
            result_t bi = static_cast<result_t>(b[i]);
            ab += ai * bi, a2 += square(ai), b2 += square(bi);
        }

        result_t result_if_zero[2][2];
        result_if_zero[0][0] = 1 - ab / (std::sqrt(a2) * std::sqrt(b2));
        result_if_zero[0][1] = result_if_zero[1][0] = 1;
        result_if_zero[1][1] = 0;
        return result_if_zero[a2 == 0][b2 == 0];
    }
};

/**
 *  @brief  Squared Euclidean (L2) distance.
 *          Square root is avoided at the end, as it won't affect the ordering.
 */
template <typename scalar_at = float, typename result_at = scalar_at> struct metric_l2sq_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t dim) const noexcept {
        result_t ab_deltas_sq{};
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : ab_deltas_sq)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != dim; ++i) {
            result_t ai = static_cast<result_t>(a[i]);
            result_t bi = static_cast<result_t>(b[i]);
            ab_deltas_sq += square(ai - bi);
        }
        return ab_deltas_sq;
    }
};

/**
 *  @brief  Hamming distance computes the number of differing bits in
 *          two arrays of integers. An example would be a textual document,
 *          tokenized and hashed into a fixed-capacity bitset.
 */
template <typename scalar_at = std::uint64_t, typename result_at = std::size_t> struct metric_hamming_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    static_assert( //
        std::is_unsigned<scalar_t>::value ||
            (std::is_enum<scalar_t>::value && std::is_unsigned<typename std::underlying_type<scalar_t>::type>::value),
        "Hamming distance requires unsigned integral words");

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t words) const noexcept {
        constexpr std::size_t bits_per_word_k = sizeof(scalar_t) * CHAR_BIT;
        result_t matches{};
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : matches)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != words; ++i)
            matches += std::bitset<bits_per_word_k>(a[i] ^ b[i]).count();
        return matches;
    }
};

/**
 *  @brief  Tanimoto distance is the intersection over bitwise union.
 *          Often used in chemistry and biology to compare molecular fingerprints.
 */
template <typename scalar_at = std::uint64_t, typename result_at = float> struct metric_tanimoto_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    static_assert( //
        std::is_unsigned<scalar_t>::value ||
            (std::is_enum<scalar_t>::value && std::is_unsigned<typename std::underlying_type<scalar_t>::type>::value),
        "Tanimoto distance requires unsigned integral words");
    static_assert(std::is_floating_point<result_t>::value, "Tanimoto distance will be a fraction");

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t words) const noexcept {
        constexpr std::size_t bits_per_word_k = sizeof(scalar_t) * CHAR_BIT;
        result_t and_count{};
        result_t or_count{};
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : and_count, or_count)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != words; ++i) {
            and_count += std::bitset<bits_per_word_k>(a[i] & b[i]).count();
            or_count += std::bitset<bits_per_word_k>(a[i] | b[i]).count();
        }
        return 1 - result_t(and_count) / or_count;
    }
};

/**
 *  @brief  Sorensen-Dice or F1 distance is the intersection over bitwise union.
 *          Often used in chemistry and biology to compare molecular fingerprints.
 */
template <typename scalar_at = std::uint64_t, typename result_at = float> struct metric_sorensen_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    static_assert( //
        std::is_unsigned<scalar_t>::value ||
            (std::is_enum<scalar_t>::value && std::is_unsigned<typename std::underlying_type<scalar_t>::type>::value),
        "Sorensen-Dice distance requires unsigned integral words");
    static_assert(std::is_floating_point<result_t>::value, "Sorensen-Dice distance will be a fraction");

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t words) const noexcept {
        constexpr std::size_t bits_per_word_k = sizeof(scalar_t) * CHAR_BIT;
        result_t and_count{};
        result_t any_count{};
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : and_count, any_count)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != words; ++i) {
            and_count += std::bitset<bits_per_word_k>(a[i] & b[i]).count();
            any_count += std::bitset<bits_per_word_k>(a[i]).count() + std::bitset<bits_per_word_k>(b[i]).count();
        }
        return 1 - 2 * result_t(and_count) / any_count;
    }
};

/**
 *  @brief  Counts the number of matching elements in two unique sorted sets.
 *          Can be used to compute the similarity between two textual documents
 *          using the IDs of tokens present in them.
 *          Similar to `metric_tanimoto_gt` for dense representations.
 */
template <typename scalar_at = std::int32_t, typename result_at = float> struct metric_jaccard_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    static_assert(!std::is_floating_point<scalar_t>::value, "Jaccard distance requires integral scalars");
    static_assert(std::is_floating_point<result_t>::value, "Jaccard distance returns a fraction");

    inline result_t operator()( //
        scalar_t const* a, scalar_t const* b, std::size_t a_length, std::size_t b_length) const noexcept {
        std::size_t intersection{};
        std::size_t i{};
        std::size_t j{};
        while (i != a_length && j != b_length) {
            scalar_t ai = a[i];
            scalar_t bj = b[j];
            intersection += ai == bj;
            i += ai < bj;
            j += ai >= bj;
        }
        return 1 - static_cast<result_t>(intersection) / (a_length + b_length - intersection);
    }
};

/**
 *  @brief  Measures Pearson Correlation between two sequences in a single pass.
 */
template <typename scalar_at = float, typename result_at = float> struct metric_pearson_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t dim) const noexcept {
        // The correlation coefficient can't be defined for one or zero-dimensional data.
        if (dim <= 1)
            return 0;
        // Conventional Pearson Correlation Coefficient definiton subtracts the mean value of each
        // sequence from each element, before dividing them. WikiPedia article suggests a convenient
        // single-pass algorithm for calculating sample correlations, though depending on the numbers
        // involved, it can sometimes be numerically unstable.
        result_t a_sum{}, b_sum{}, ab_sum{};
        result_t a_sq_sum{}, b_sq_sum{};
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : a_sum, b_sum, ab_sum, a_sq_sum, b_sq_sum)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != dim; ++i) {
            result_t ai = static_cast<result_t>(a[i]);
            result_t bi = static_cast<result_t>(b[i]);
            a_sum += ai;
            b_sum += bi;
            ab_sum += ai * bi;
            a_sq_sum += ai * ai;
            b_sq_sum += bi * bi;
        }
        result_t denom = (dim * a_sq_sum - a_sum * a_sum) * (dim * b_sq_sum - b_sum * b_sum);
        // could be negative
        if (denom <= 0)
            return 0;
        result_t corr = dim * ab_sum - a_sum * b_sum;
        denom = std::sqrt(denom);
        // The normal Pearson correlation value is between -1 and 1, but we are looking for a distance.
        // So instead of returning `corr / denom`, we return `1 - corr / denom`.
        return 1 - corr / denom;
    }
};

/**
 *  @brief  Measures Jensen-Shannon Divergence between two probability distributions.
 */
template <typename scalar_at = float, typename result_at = float> struct metric_divergence_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;

    inline result_t operator()(scalar_t const* p, scalar_t const* q, std::size_t dim) const noexcept {
        result_t kld_pm{}, kld_qm{};
        result_t epsilon = std::numeric_limits<result_t>::epsilon();
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : kld_pm, kld_qm)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != dim; ++i) {
            result_t pi = static_cast<result_t>(p[i]);
            result_t qi = static_cast<result_t>(q[i]);
            result_t mi = (pi + qi) / 2 + epsilon;
            kld_pm += pi * std::log((pi + epsilon) / mi);
            kld_qm += qi * std::log((qi + epsilon) / mi);
        }
        return (kld_pm + kld_qm) / 2;
    }
};

/**
 *  @brief  Cosine (Angular) distance for signed 8-bit integers using 16-bit intermediates.
 */
struct metric_cos_i8_t {
    using scalar_t = i8_t;
    using result_t = f32_t;

    inline result_t operator()(i8_t const* a, i8_t const* b, std::size_t dim) const noexcept {
        std::int32_t ab{}, a2{}, b2{};
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : ab, a2, b2)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != dim; i++) {
            std::int16_t ai{a[i]};
            std::int16_t bi{b[i]};
            ab += ai * bi;
            a2 += square(ai);
            b2 += square(bi);
        }
        result_t a2f = std::sqrt(static_cast<result_t>(a2));
        result_t b2f = std::sqrt(static_cast<result_t>(b2));
        return (ab != 0) ? (1.f - ab / (a2f * b2f)) : 0;
    }
};

/**
 *  @brief  Squared Euclidean (L2) distance for signed 8-bit integers using 16-bit intermediates.
 *          Square root is avoided at the end, as it won't affect the ordering.
 */
struct metric_l2sq_i8_t {
    using scalar_t = i8_t;
    using result_t = f32_t;

    inline result_t operator()(i8_t const* a, i8_t const* b, std::size_t dim) const noexcept {
        std::int32_t ab_deltas_sq{};
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : ab_deltas_sq)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != dim; i++)
            ab_deltas_sq += square(std::int16_t(a[i]) - std::int16_t(b[i]));
        return static_cast<result_t>(ab_deltas_sq);
    }
};

/**
 *  @brief  Cosine (Angular) distance for unsigned 8-bit integers using 32-bit intermediates.
 */
struct metric_cos_u8_t {
    using scalar_t = u8_t;
    using result_t = f32_t;

    inline result_t operator()(u8_t const* a, u8_t const* b, std::size_t dim) const noexcept {
        std::int64_t ab{}, a2{}, b2{};
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : ab, a2, b2)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != dim; i++) {
            std::int32_t ai{a[i]};
            std::int32_t bi{b[i]};
            ab += ai * bi;
            a2 += square(ai);
            b2 += square(bi);
        }
        result_t a2f = std::sqrt(static_cast<result_t>(a2));
        result_t b2f = std::sqrt(static_cast<result_t>(b2));
        return (ab != 0) ? (1.f - ab / (a2f * b2f)) : 0;
    }
};

/**
 *  @brief  Squared Euclidean (L2) distance for unsigned 8-bit integers using 32-bit intermediates.
 */
struct metric_l2sq_u8_t {
    using scalar_t = u8_t;
    using result_t = f32_t;

    inline result_t operator()(u8_t const* a, u8_t const* b, std::size_t dim) const noexcept {
        std::int32_t ab_deltas_sq{};
#if USEARCH_USE_OPENMP
#pragma omp simd reduction(+ : ab_deltas_sq)
#elif defined(USEARCH_DEFINED_CLANG)
#pragma clang loop vectorize(enable)
#elif defined(USEARCH_DEFINED_GCC)
#pragma GCC ivdep
#endif
        for (std::size_t i = 0; i != dim; i++)
            ab_deltas_sq += square(std::int32_t(a[i]) - std::int32_t(b[i]));
        return static_cast<result_t>(ab_deltas_sq);
    }
};

/**
 *  @brief  Haversine distance for the shortest distance between two nodes on
 *          the surface of a 3D sphere, defined with latitude and longitude.
 */
template <typename scalar_at = float, typename result_at = scalar_at> struct metric_haversine_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    static_assert(!std::is_integral<scalar_t>::value && !std::is_same<scalar_t, f16_t>::value,
                  "Latitude and longitude must be floating-node");

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t = 2) const noexcept {
        result_t lat_a = a[0], lon_a = a[1];
        result_t lat_b = b[0], lon_b = b[1];

        result_t lat_delta = angle_to_radians<result_t>(lat_b - lat_a) / 2;
        result_t lon_delta = angle_to_radians<result_t>(lon_b - lon_a) / 2;

        result_t converted_lat_a = angle_to_radians<result_t>(lat_a);
        result_t converted_lat_b = angle_to_radians<result_t>(lat_b);

        result_t x = square(std::sin(lat_delta)) + //
                     std::cos(converted_lat_a) * std::cos(converted_lat_b) * square(std::sin(lon_delta));

        return 2 * std::asin(std::sqrt(x));
    }
};

using distance_punned_t = float;
using span_punned_t = span_gt<byte_t const>;

} // namespace usearch
} // namespace unum
