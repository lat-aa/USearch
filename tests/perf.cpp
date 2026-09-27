/**
 *  @file       perf.cpp
 *  @brief      固定种子小规模吞吐门禁：输出 JSON，供 performance.yml 对照 baseline。
 *
 *  度量 add / search / search_batch 的 QPS；失败时非零退出。不替代完整 bench_cpp。
 */
#include <dense/dense.hpp>

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using namespace unum::usearch;

static double qps(std::size_t ops, double seconds) {
    return seconds > 0 ? static_cast<double>(ops) / seconds : 0.0;
}

int main() {
    constexpr std::size_t dim = 32;
    constexpr std::size_t n = 2000;
    constexpr std::size_t nq = 100;
    constexpr std::size_t k = 10;
    constexpr std::size_t threads = 4;

    metric_punned_t metric(dim, metric_kind_t::l2sq_k, scalar_kind_t::f32_k);
    auto made = index_dense_t::make(metric);
    if (!made)
        return 1;
    index_dense_t index = std::move(made.index);
    if (!index.try_reserve({n + 16, threads}))
        return 2;

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    std::vector<float> data(n * dim);
    for (float& x : data)
        x = dist(rng);

    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        auto r = index.add(static_cast<std::int64_t>(i), data.data() + i * dim);
        if (!r) {
            r.error.release();
            return 3;
        }
        r.error.release();
    }
    double add_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::vector<float> queries(nq * dim);
    for (float& x : queries)
        x = dist(rng);

    t0 = std::chrono::steady_clock::now();
    for (std::size_t q = 0; q < nq; ++q) {
        auto r = index.search(queries.data() + q * dim, k);
        if (!r || r.size() == 0) {
            if (r)
                ;
            else
                r.error.release();
            return 4;
        }
    }
    double search_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    executor_default_t exec(threads);
    t0 = std::chrono::steady_clock::now();
    auto batch = index.search_batch(queries.data(), nq, k, exec);
    double batch_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!batch) {
        batch.error.release();
        return 5;
    }

    double add_qps = qps(n, add_s);
    double search_qps = qps(nq, search_s);
    double batch_qps = qps(nq, batch_s);

    std::printf(
        "{\"add_qps\":%.3f,\"search_qps\":%.3f,\"batch_qps\":%.3f,\"vectors\":%zu,\"queries\":%zu,\"wanted\":%zu}\n",
        add_qps, search_qps, batch_qps, n, nq, k);
    return 0;
}
