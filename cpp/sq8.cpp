/**
 *  @file       sq8.cpp
 *  @brief      SQ8 对称 int8 影子：量化、粗分、候选证明（移植自 Apex sq8.rs）。
 */

#include "api.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace api {
namespace sq8 {

std::size_t candidateBudget(std::size_t k) { return (std::max)(std::size_t{1024}, 16 * k); }

float epsFromMaxScale(std::size_t dim, float maxScale) {
    return std::sqrt(static_cast<float>(dim)) * epsSlack * (maxScale / 254.0f + 1.0f / 65534.0f);
}

float quantizeRow(float const* row, std::int8_t* dst, std::size_t dim) {
    for (std::size_t i = 0; i != dim; ++i) {
        if (!std::isfinite(row[i])) {
            for (std::size_t j = 0; j != dim; ++j)
                dst[j] = 0;
            return -1.0f;
        }
    }
    float s = 0.0f;
    for (std::size_t i = 0; i != dim; ++i)
        s = (std::max)(s, std::fabs(row[i]));
    if (s > 0.0f) {
        float inv = 127.0f / s;
        for (std::size_t i = 0; i != dim; ++i) {
            float v = std::round(row[i] * inv);
            v = (std::max)(-127.0f, (std::min)(127.0f, v));
            dst[i] = static_cast<std::int8_t>(v);
        }
    } else {
        for (std::size_t i = 0; i != dim; ++i)
            dst[i] = 0;
    }
    return s;
}

void buildShadow(float const* data, std::size_t n, std::size_t dim, std::vector<std::int8_t>& q8,
                 std::vector<float>& qscale, float& eps) {
    q8.clear();
    qscale.clear();
    eps = 0.0f;
    if (n == 0 || dim == 0)
        return;
    q8.assign(n * dim, 0);
    qscale.assign(n, 0.0f);
    float maxScale = 0.0f;
    for (std::size_t i = 0; i != n; ++i) {
        float s = quantizeRow(data + i * dim, q8.data() + i * dim, dim);
        qscale[i] = s;
        if (s > maxScale)
            maxScale = s;
    }
    eps = epsFromMaxScale(dim, maxScale);
}

std::vector<std::int16_t> quantizeQueryI16(float const* q, std::size_t dim) {
    std::vector<std::int16_t> out(dim, 0);
    for (std::size_t i = 0; i != dim; ++i) {
        float v = std::round(q[i] * 32767.0f);
        if (!std::isfinite(v))
            out[i] = 0;
        else {
            v = (std::max)(-32767.0f, (std::min)(32767.0f, v));
            out[i] = static_cast<std::int16_t>(v);
        }
    }
    return out;
}

std::int32_t i8DotScalar(std::int8_t const* doc, std::int16_t const* q, std::size_t dim) {
    std::int32_t s = 0;
    for (std::size_t i = 0; i != dim; ++i)
        s += static_cast<std::int32_t>(doc[i]) * static_cast<std::int32_t>(q[i]);
    return s;
}

#if defined(__AVX2__)
#include <immintrin.h>
std::int32_t i8DotAvx2(std::int8_t const* doc, std::int16_t const* q, std::size_t dim) {
    __m256i acc = _mm256_setzero_si256();
    std::size_t i = 0;
    for (; i + 16 <= dim; i += 16) {
        __m128i d8 = _mm_loadu_si128(reinterpret_cast<__m128i const*>(doc + i));
        __m256i d16 = _mm256_cvtepi8_epi16(d8);
        __m256i q16 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(q + i));
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(d16, q16));
    }
    alignas(32) std::int32_t lane[8];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(lane), acc);
    std::int32_t s = lane[0] + lane[1] + lane[2] + lane[3] + lane[4] + lane[5] + lane[6] + lane[7];
    for (; i < dim; ++i)
        s += static_cast<std::int32_t>(doc[i]) * static_cast<std::int32_t>(q[i]);
    return s;
}
#endif

std::int32_t i8Dot(std::int8_t const* doc, std::int16_t const* q, std::size_t dim) {
#if defined(__AVX2__)
    return i8DotAvx2(doc, q, dim);
#else
    return i8DotScalar(doc, q, dim);
#endif
}

float coarseScore(std::int32_t dot, float scale) {
    if (scale < 0.0f)
        return -std::numeric_limits<float>::infinity();
    if (scale == 0.0f)
        return 0.0f;
    return static_cast<float>(dot) * scale * i8i16Inv;
}

bool selectCandidates(float const* est, std::size_t n, std::size_t k, float eps,
                      std::vector<std::size_t>& out) {
    out.clear();
    if (k == 0 || n < k)
        return false;
    std::vector<float> sorted(est, est + n);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k - 1), sorted.end(),
                     [](float a, float b) { return a > b; });
    float tau = sorted[k - 1];
    if (!std::isfinite(tau))
        return false;
    float cut = tau - 2.0f * eps;
    for (std::size_t i = 0; i != n; ++i)
        if (est[i] >= cut)
            out.push_back(i);
    if (out.size() > candidateBudget(k)) {
        out.clear();
        return false;
    }
    return true;
}

float dot8(float const* a, float const* b, std::size_t dim) {
    float acc[8] = {};
    std::size_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        for (std::size_t j = 0; j != 8; ++j)
            acc[j] += a[i + j] * b[i + j];
    }
    float s = ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
    for (; i < dim; ++i)
        s += a[i] * b[i];
    return s;
}

std::vector<std::pair<std::size_t, float>> selectTopKExact(
    std::size_t n, std::size_t k, float threshold, std::size_t const* rows, std::size_t nrows,
    std::function<float(std::size_t)> scoreAt) {
    std::vector<std::pair<std::size_t, float>> result;
    if (k == 0 || n == 0)
        return result;
    // 升序缓冲 (score, idx)，满后踢掉最小；最后反转。
    std::vector<std::pair<float, std::size_t>> best;
    best.reserve(k);
    auto consider = [&](std::size_t i) {
        float score = scoreAt(i);
        if (score < threshold || std::isnan(score))
            return;
        if (best.size() == k) {
            if (score <= best.front().first)
                return;
            best.erase(best.begin());
        }
        auto at = std::lower_bound(best.begin(), best.end(), score,
                                   [](std::pair<float, std::size_t> const& p, float s) { return p.first < s; });
        best.insert(at, {score, i});
    };
    if (rows && nrows) {
        for (std::size_t r = 0; r != nrows; ++r)
            consider(rows[r]);
    } else {
        for (std::size_t i = 0; i != n; ++i)
            consider(i);
    }
    std::reverse(best.begin(), best.end());
    result.reserve(best.size());
    for (auto const& p : best)
        result.emplace_back(p.second, p.first);
    return result;
}

} // namespace sq8
} // namespace api
