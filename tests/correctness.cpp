/**
 *  @file       test_correctness.cpp
 *  @brief      差分正确性：暴力精确 top-k vs HNSW；SQ8 候选证明 vs f32 精排。
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

#include <usearch/index_dense.hpp>

using namespace unum::usearch;
using dense_t = index_dense_gt<>;

static int gFails = 0;

static void expectTrue(bool cond, char const* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++gFails;
    }
}

static void l2Normalize(float* v, std::size_t dim) {
    float n = 0;
    for (std::size_t i = 0; i != dim; ++i)
        n += v[i] * v[i];
    n = std::sqrt((std::max)(n, 1e-12f));
    for (std::size_t i = 0; i != dim; ++i)
        v[i] /= n;
}

static float dot(float const* a, float const* b, std::size_t dim) {
    float s = 0;
    for (std::size_t i = 0; i != dim; ++i)
        s += a[i] * b[i];
    return s;
}

/** 精确 top-k（余弦相似度最大）；返回行下标。 */
static std::vector<std::size_t> exactTopK(std::vector<float> const& data, std::size_t n, std::size_t dim,
                                          float const* query, std::size_t k) {
    std::vector<std::pair<float, std::size_t>> scored;
    scored.reserve(n);
    for (std::size_t i = 0; i != n; ++i)
        scored.push_back({dot(data.data() + i * dim, query, dim), i});
    std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(k), scored.end(),
                      [](auto const& a, auto const& b) { return a.first > b.first; });
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i != k; ++i)
        out.push_back(scored[i].second);
    std::sort(out.begin(), out.end());
    return out;
}

static void testAnnMatchesExact() {
    constexpr std::size_t dim = 32;
    constexpr std::size_t n = 64;
    constexpr std::size_t k = 8;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);

    std::vector<float> data(n * dim);
    for (float& x : data)
        x = dist(rng);
    for (std::size_t i = 0; i != n; ++i)
        l2Normalize(data.data() + i * dim, dim);

    auto made = dense_t::make(metric_punned_t(dim, metric_kind_t::cos_k));
    expectTrue(static_cast<bool>(made), "make index");
    dense_t index = std::move(made.index);
    expectTrue(index.try_reserve(index_limits_t(n + 16)), "reserve");
    // 高扩展：小库上逼近精确。
    index.change_expansion_search(n);
    index.change_expansion_add(n);

    for (std::size_t i = 0; i != n; ++i)
        expectTrue(static_cast<bool>(index.add(static_cast<default_key_t>(i + 1), data.data() + i * dim)), "add");

    float query[dim];
    for (float& x : query)
        x = dist(rng);
    l2Normalize(query, dim);

    auto exact = exactTopK(data, n, dim, query, k);
    auto results = index.search(query, k);
    expectTrue(static_cast<bool>(results), "search ok");
    std::vector<default_key_t> keys(k);
    std::vector<float> distances(k);
    std::size_t got = results.dump_to(keys.data(), distances.data());
    expectTrue(got == k, "k results");

    std::vector<std::size_t> ann;
    for (std::size_t i = 0; i != got; ++i) {
        auto key = static_cast<std::uint64_t>(keys[i]);
        expectTrue(key >= 1 && key <= n, "key range");
        ann.push_back(static_cast<std::size_t>(key - 1));
    }
    std::sort(ann.begin(), ann.end());
    expectTrue(ann == exact, "HNSW top-k equals exact on tiny corpus with high ef");
}

// --- 内联 SQ8 证明核心（与 sq8.cpp 同构），避免链完整 api ---
namespace {

constexpr float kEpsSlack = 1.0001f;
constexpr float kI8I16Inv = 1.0f / (127.0f * 32767.0f);

float quantizeRow(float const* row, std::int8_t* dst, std::size_t dim) {
    float s = 0;
    for (std::size_t i = 0; i != dim; ++i) {
        if (!std::isfinite(row[i])) {
            std::fill(dst, dst + dim, std::int8_t{0});
            return -1.f;
        }
        s = (std::max)(s, std::fabs(row[i]));
    }
    if (s > 0) {
        float inv = 127.f / s;
        for (std::size_t i = 0; i != dim; ++i) {
            float v = std::round(row[i] * inv);
            v = (std::max)(-127.f, (std::min)(127.f, v));
            dst[i] = static_cast<std::int8_t>(v);
        }
    } else {
        std::fill(dst, dst + dim, std::int8_t{0});
    }
    return s;
}

std::int32_t i8Dot(std::int8_t const* doc, std::int16_t const* q, std::size_t dim) {
    std::int32_t s = 0;
    for (std::size_t i = 0; i != dim; ++i)
        s += static_cast<std::int32_t>(doc[i]) * static_cast<std::int32_t>(q[i]);
    return s;
}

float coarseScore(std::int32_t d, float scale) {
    if (scale < 0)
        return -std::numeric_limits<float>::infinity();
    if (scale == 0)
        return 0;
    return static_cast<float>(d) * scale * kI8I16Inv;
}

} // namespace

static void testSq8CandidatesContainExact() {
    constexpr std::size_t dim = 64;
    constexpr std::size_t n = 128;
    constexpr std::size_t k = 5;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);

    std::vector<float> data(n * dim);
    for (float& x : data)
        x = dist(rng);
    for (std::size_t i = 0; i != n; ++i)
        l2Normalize(data.data() + i * dim, dim);

    std::vector<std::int8_t> q8(n * dim);
    std::vector<float> scales(n);
    float maxScale = 0;
    for (std::size_t i = 0; i != n; ++i) {
        scales[i] = quantizeRow(data.data() + i * dim, q8.data() + i * dim, dim);
        maxScale = (std::max)(maxScale, scales[i]);
    }
    float eps = std::sqrt(static_cast<float>(dim)) * kEpsSlack * (maxScale / 254.f + 1.f / 65534.f);

    float query[dim];
    for (float& x : query)
        x = dist(rng);
    l2Normalize(query, dim);

    std::vector<std::int16_t> q16(dim);
    for (std::size_t i = 0; i != dim; ++i) {
        float v = std::round(query[i] * 32767.f);
        v = (std::max)(-32767.f, (std::min)(32767.f, v));
        q16[i] = static_cast<std::int16_t>(v);
    }

    std::vector<float> est(n);
    for (std::size_t i = 0; i != n; ++i)
        est[i] = coarseScore(i8Dot(q8.data() + i * dim, q16.data(), dim), scales[i]);

    std::vector<float> sorted = est;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k - 1), sorted.end(),
                     [](float a, float b) { return a > b; });
    float tau = sorted[k - 1];
    float cut = tau - 2.f * eps;
    std::vector<std::size_t> cand;
    for (std::size_t i = 0; i != n; ++i)
        if (est[i] >= cut)
            cand.push_back(i);

    auto exact = exactTopK(data, n, dim, query, k);
    for (auto row : exact) {
        bool found = std::find(cand.begin(), cand.end(), row) != cand.end();
        expectTrue(found, "SQ8 candidate set must contain every exact top-k row");
    }

    // 精排：候选上 f32 top-k 必须与全库 exact 一致。
    std::vector<std::pair<float, std::size_t>> refined;
    for (auto i : cand)
        refined.push_back({dot(data.data() + i * dim, query, dim), i});
    std::sort(refined.begin(), refined.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    std::vector<std::size_t> refinedTop;
    for (std::size_t i = 0; i < k && i < refined.size(); ++i)
        refinedTop.push_back(refined[i].second);
    std::sort(refinedTop.begin(), refinedTop.end());
    expectTrue(refinedTop == exact, "f32 refine on SQ8 candidates equals exact top-k");
}

int main() {
    testAnnMatchesExact();
    testSq8CandidatesContainExact();
    if (gFails) {
        std::fprintf(stderr, "%d correctness check(s) failed\n", gFails);
        return 1;
    }
    std::printf("correctness ok\n");
    return 0;
}
