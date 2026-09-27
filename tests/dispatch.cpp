/**
 *  @file  tests/dispatch.cpp
 *  @brief L0 NumKong 分发阶梯：serial mask、主机 ∩ 请求、typed miss、并发 init。
 */

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if !USEARCH_USE_NUMKONG
#error "test_numkong_dispatch requires USEARCH_USE_NUMKONG=1"
#endif

#include <numkong/numkong.h>
#include <plugins/plugins.hpp>

using namespace unum::usearch;

namespace {

int g_fails = 0;

void expect(bool ok, char const* msg) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

struct kind_dtype_t {
    nk_kernel_kind_t kind;
    nk_dtype_t dtype;
    char const* name;
};

// Metrics wired through USearch `configure_with_numkong`.
kind_dtype_t const k_wired[] = {
    {nk_kernel_dot_k, nk_f32_k, "dot_f32"},
    {nk_kernel_angular_k, nk_f32_k, "angular_f32"},
    {nk_kernel_sqeuclidean_k, nk_f32_k, "sqeuclidean_f32"},
    {nk_kernel_hamming_k, nk_u1_k, "hamming_u1"},
    {nk_kernel_jaccard_k, nk_u1_k, "jaccard_u1"},
};

void test_serial_ladder() {
    nk_dispatch_table_update(nk_cap_serial_k);
    for (auto const& e : k_wired) {
        nk_kernel_punned_t kernel = nullptr;
        nk_capability_t got = 0;
        nk_find_kernel_punned(e.kind, e.dtype, nk_cap_serial_k, &kernel, &got);
        expect(kernel != nullptr, e.name);
        expect(got == nk_cap_serial_k, "serial capability");
        if (kernel && e.dtype == nk_f32_k) {
            float a[8] = {1, 0, 0, 0, 0, 0, 0, 0};
            float b[8] = {1, 0, 0, 0, 0, 0, 0, 0};
            float out = 0;
            auto dense = reinterpret_cast<nk_metric_dense_punned_t>(kernel);
            dense(a, b, 8, &out);
            expect(std::isfinite(out), "serial kernel finite");
        }
    }
    nk_dispatch_table_update(nk_capabilities());
}

void test_host_intersect() {
    nk_capability_t host = nk_capabilities();
    nk_capability_t compiled = nk_capabilities_compiled();
    nk_capability_t request = host & compiled;
    expect((request & nk_cap_serial_k) != 0, "host always has serial");

    nk_kernel_punned_t kernel = nullptr;
    nk_capability_t got = 0;
    nk_find_kernel_punned(nk_kernel_angular_k, nk_f32_k, request, &kernel, &got);
    expect(kernel != nullptr, "host angular_f32");
    expect((got & request) == got && got != 0, "returned capability ⊆ request");

    float a[16], b[16];
    for (int i = 0; i < 16; ++i) {
        a[i] = static_cast<float>(i);
        b[i] = static_cast<float>(15 - i);
    }
    float out = 0;
    reinterpret_cast<nk_metric_dense_punned_t>(kernel)(a, b, 16, &out);
    expect(std::isfinite(out), "host kernel finite");
}

void test_typed_miss() {
    // Exclude serial and every compiled SIMD bit → forced miss.
    // Stubs are library-internal (hidden visibility); assert ABI by safe invoke, not pointer identity.
    nk_capability_t miss_mask = 0;
    nk_kernel_kind_t kinds[] = {
        nk_kernel_angular_k,         nk_kernel_each_sum_k,        nk_kernel_dots_pack_k,
        nk_kernel_dots_packed_k,     nk_kernel_dots_packed_size_k, nk_kernel_each_fma_k,
        nk_kernel_reduce_moments_k,
    };
    for (nk_kernel_kind_t kind : kinds) {
        nk_kernel_punned_t kernel = nullptr;
        nk_capability_t got = nk_cap_any_k;
        nk_find_kernel_punned(kind, nk_f32_k, miss_mask, &kernel, &got);
        expect(got == 0, "miss capability zero");
        expect(kernel != nullptr, "miss returns typed stub (non-null)");
        if (!kernel)
            continue;
        // Invoke stub: must not crash; dense/each/pack fill error pattern or return 0 size.
        if (kind == nk_kernel_dots_packed_size_k) {
            auto size_fn = reinterpret_cast<nk_size_t (*)(nk_size_t, nk_size_t)>(kernel);
            expect(size_fn(8, 8) == 0, "packed_size miss returns 0");
        } else if (kind == nk_kernel_dots_pack_k) {
            auto pack = reinterpret_cast<void (*)(void const*, nk_size_t, nk_size_t, nk_size_t, void*)>(kernel);
            char buf[64] = {};
            pack(buf, 1, 8, 8, buf);
        } else if (kind == nk_kernel_dots_packed_k) {
            auto dots = reinterpret_cast<void (*)(void const*, void const*, void*, nk_size_t, nk_size_t, nk_size_t,
                                                  nk_size_t, nk_size_t)>(kernel);
            float a[8] = {}, c[8] = {};
            dots(a, a, c, 1, 1, 8, 8, sizeof(float));
        } else if (kind == nk_kernel_each_sum_k || kind == nk_kernel_each_fma_k) {
            float a[4] = {1, 2, 3, 4}, b[4] = {1, 1, 1, 1}, y[4] = {};
            if (kind == nk_kernel_each_sum_k) {
                auto fn = reinterpret_cast<void (*)(void const*, void const*, nk_size_t, void*)>(kernel);
                fn(a, b, 4, y);
            } else {
                float alpha = 1, beta = 1, c[4] = {};
                auto fn = reinterpret_cast<void (*)(void const*, void const*, void const*, nk_size_t, void const*,
                                                    void const*, void*)>(kernel);
                fn(a, b, c, 4, &alpha, &beta, y);
            }
        } else if (kind == nk_kernel_reduce_moments_k) {
            float data[4] = {1, 2, 3, 4}, sum = 0, sumsq = 0;
            auto fn = reinterpret_cast<void (*)(void const*, nk_size_t, nk_size_t, void*, void*)>(kernel);
            fn(data, 4, sizeof(float), &sum, &sumsq);
        } else {
            float a[8] = {}, b[8] = {}, out = 0;
            auto dense = reinterpret_cast<nk_metric_dense_punned_t>(kernel);
            dense(a, b, 8, &out);
        }
    }
}

void test_concurrent_init() {
    constexpr int n = 8;
    std::atomic<int> ready{0};
    std::vector<nk_capability_t> caps(static_cast<std::size_t>(n));
    std::vector<float> dists(static_cast<std::size_t>(n));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(n));

    float a[32], b[32];
    for (int i = 0; i < 32; ++i) {
        a[i] = 0.01f * static_cast<float>(i);
        b[i] = 0.02f * static_cast<float>(31 - i);
    }

    for (int t = 0; t < n; ++t) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < n) {
            }
            caps[static_cast<std::size_t>(t)] = nk_capabilities();
            metric_punned_t m = metric_punned_t::builtin(32, metric_kind_t::cos_k, scalar_kind_t::f32_k);
            dists[static_cast<std::size_t>(t)] =
                m(reinterpret_cast<byte_t const*>(a), reinterpret_cast<byte_t const*>(b));
        });
    }
    for (auto& th : threads)
        th.join();

    for (int t = 1; t < n; ++t) {
        expect(caps[static_cast<std::size_t>(t)] == caps[0], "concurrent caps equal");
        expect(dists[static_cast<std::size_t>(t)] == dists[0], "concurrent cos equal");
    }
    expect(std::isfinite(dists[0]), "concurrent cos finite");
}

} // namespace

int main() {
    // Ensure library path is live before mutating dispatch table.
    (void)nk_capabilities();

    test_serial_ladder();
    test_host_intersect();
    test_typed_miss();
    test_concurrent_init();

    // Hardware acceleration string should be non-empty (serial counts).
    char const* accel = hardware_acceleration_available();
    expect(accel != nullptr && accel[0] != '\0', "hardware_acceleration_available");

    if (g_fails) {
        std::fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    std::puts("test_numkong_dispatch ok");
    return 0;
}
