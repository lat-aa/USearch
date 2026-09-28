/**
 *  @file       capi.cpp
 *  @brief      C API usearch_search_batch 冒烟：小索引多查询，断言每行有命中。
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "../c/usearch.h"
}

int main() {
    usearch_error_t error = nullptr;
    usearch_init_options_t opts;
    std::memset(&opts, 0, sizeof(opts));
    opts.dimensions = 8;
    opts.metric_kind = usearch_metric_l2sq_k;
    opts.quantization = usearch_scalar_f32_k;
    opts.connectivity = 8;
    opts.expansion_add = 16;
    opts.expansion_search = 16;

    usearch_index_t index = usearch_init(&opts, &error);
    assert(index && !error);
    usearch_reserve(index, 64, &error);
    assert(!error);

    float vec[8];
    for (int i = 0; i < 32; ++i) {
        for (int d = 0; d < 8; ++d)
            vec[d] = static_cast<float>((i + d) % 17);
        usearch_add(index, static_cast<usearch_key_t>(i + 1), vec, usearch_scalar_f32_k, &error);
        assert(!error);
    }

    constexpr size_t nq = 4;
    constexpr size_t k = 3;
    std::vector<float> queries(nq * 8);
    for (size_t q = 0; q < nq; ++q)
        for (size_t d = 0; d < 8; ++d)
            queries[q * 8 + d] = static_cast<float>((q + d) % 17);

    std::vector<usearch_key_t> keys(nq * k);
    std::vector<usearch_distance_t> dists(nq * k);
    std::vector<size_t> counts(nq, 0);

    size_t total =
        usearch_search_batch(index, queries.data(), nq, usearch_scalar_f32_k, k, keys.data(), dists.data(),
                             counts.data(), &error);
    assert(!error);
    assert(total > 0);
    for (size_t q = 0; q < nq; ++q)
        assert(counts[q] > 0 && counts[q] <= k);

    usearch_free(index, &error);
    std::puts("capi: ok");
    return 0;
}
