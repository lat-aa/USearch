/**
 *  @file       batch.cpp
 *  @brief      search_batch / reclaim 冒烟：小规模随机向量，断言形状与回收后 size。
 */
#include <dense/dense.hpp>

#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

using namespace unum::usearch;

static void fill_rand(float* dst, std::size_t n, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    for (std::size_t i = 0; i != n; ++i)
        dst[i] = dist(rng);
}

int main() {
    constexpr std::size_t dim = 16;
    constexpr std::size_t n = 200;
    constexpr std::size_t nq = 8;
    constexpr std::size_t k = 5;

    metric_punned_t metric(dim, metric_kind_t::l2sq_k, scalar_kind_t::f32_k);
    auto made = index_dense_t::make(metric);
    assert(made);
    index_dense_t index = std::move(made.index);
    assert(index.try_reserve({n + 10, 4}));

    std::mt19937 rng(42);
    std::vector<float> vec(dim);
    for (std::size_t i = 0; i < n; ++i) {
        fill_rand(vec.data(), dim, rng);
        auto r = index.add(static_cast<std::int64_t>(i), vec.data());
        assert(r);
        r.error.release();
    }
    assert(index.size() == n);

    std::vector<float> queries(nq * dim);
    fill_rand(queries.data(), queries.size(), rng);

    executor_default_t exec(4);
    auto batch = index.search_batch(queries.data(), nq, k, exec);
    assert(batch);
    assert(batch.queries == nq);
    assert(batch.wanted == k);
    assert(batch.counts.size() == nq);
    assert(batch.keys.size() == nq * k);
    for (std::size_t q = 0; q < nq; ++q)
        assert(batch.counts[q] > 0 && batch.counts[q] <= k);

    for (std::size_t i = 0; i < n / 3; ++i) {
        auto rem = index.remove(static_cast<std::int64_t>(i));
        rem.error.release();
    }
    std::size_t live = index.size();
    auto rec = index.reclaim();
    assert(rec);
    assert(rec.after_size == live);
    assert(index.size() == live);

    std::puts("batch: ok");
    return 0;
}
