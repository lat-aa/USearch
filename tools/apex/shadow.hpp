/**
 *  @file       shadow.hpp
 *  @brief      SQ8 对称 int8 影子索引：纯量化/打分（inline，可单测）；内核在 shadow.cpp。
 *
 *  纯函数集中在此（无状态、可被 apexshadow 直接验证）；带 ISA 分发的内核留在 shadow.cpp。
 */
#pragma once

#include "types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace api {
namespace sq8 {

constexpr float epsSlack = 1.0001f;
constexpr float i8i16Inv = 1.0f / (127.0f * 32767.0f);

inline std::size_t candidateBudget(std::size_t k) { return (std::max)(std::size_t{1024}, 16 * k); }

inline float epsFromMaxScale(std::size_t dim, float maxScale) {
    return std::sqrt(static_cast<float>(dim)) * epsSlack * (maxScale / 254.0f + 1.0f / 65534.0f);
}

/** 逐行对称量化 |x|≤s → [-127,127]；非有限输入返回 -1 哨兵（整行清零）。 */
inline float quantizeRow(float const* row, std::int8_t* dst, std::size_t dim) {
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

inline std::vector<std::int16_t> quantizeQueryI16(float const* q, std::size_t dim) {
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

inline std::int32_t i8DotScalar(std::int8_t const* doc, std::int16_t const* q, std::size_t dim) {
    std::int32_t s = 0;
    for (std::size_t i = 0; i != dim; ++i)
        s += static_cast<std::int32_t>(doc[i]) * static_cast<std::int32_t>(q[i]);
    return s;
}

/** 粗分：scale<0 为非有限行哨兵 → -inf（永不被选）。 */
inline float coarseScore(std::int32_t dot, float scale) {
    if (scale < 0.0f)
        return -std::numeric_limits<float>::infinity();
    if (scale == 0.0f)
        return 0.0f;
    return static_cast<float>(dot) * scale * i8i16Inv;
}

void buildShadow(float const* data, std::size_t n, std::size_t dim, std::vector<std::int8_t>& q8,
                 std::vector<float>& qscale, float& eps);
std::int32_t i8Dot(std::int8_t const* doc, std::int16_t const* q, std::size_t dim);
bool selectCandidates(float const* est, std::size_t n, std::size_t k, float eps, std::vector<std::size_t>& out);
float dot8(float const* a, float const* b, std::size_t dim);
std::vector<std::pair<std::size_t, float>> selectTopKExact(std::size_t n, std::size_t k, float threshold,
                                                           std::size_t const* rows, std::size_t nrows,
                                                           std::function<float(std::size_t)> scoreAt);

} // namespace sq8
} // namespace api
