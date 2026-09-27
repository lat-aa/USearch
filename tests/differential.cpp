/**
 *  @file  tests/differential.cpp
 *  @brief L1 NumKong vs serial-mask 差分 + L4 回归钉子。
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#if !USEARCH_USE_NUMKONG
#error "test_numkong_diff requires USEARCH_USE_NUMKONG=1"
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

bool close_f(float a, float b, float atol, float rtol) {
    float d = std::fabs(a - b);
    return d <= atol + rtol * (std::max)(std::fabs(a), std::fabs(b));
}

void fill_random(std::vector<float>& v, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (float& x : v)
        x = dist(rng);
}

void fill_near_dup(std::vector<float> const& src, std::vector<float>& dst, float eps, std::mt19937& rng) {
    std::uniform_real_distribution<float> noise(-eps, eps);
    dst.resize(src.size());
    for (std::size_t i = 0; i < src.size(); ++i)
        dst[i] = src[i] + noise(rng);
}

void fill_antipodal(std::vector<float> const& src, std::vector<float>& dst) {
    dst.resize(src.size());
    for (std::size_t i = 0; i < src.size(); ++i)
        dst[i] = -src[i];
}

void compare_pair(std::size_t dim, metric_kind_t kind, float const* a, float const* b, char const* tag) {
    auto m_full = metric_punned_t::builtin(dim, kind, scalar_kind_t::f32_k);
    auto m_serial = metric_punned_t::builtin_with_caps(dim, kind, scalar_kind_t::f32_k, nk_cap_serial_k);

    expect(bool(m_full) && bool(m_serial), "metrics configured");
    expect(std::strcmp(m_serial.isa_name(), "serial") == 0, "serial-mask isa");

    float d_full = m_full(reinterpret_cast<byte_t const*>(a), reinterpret_cast<byte_t const*>(b));
    float d_ser = m_serial(reinterpret_cast<byte_t const*>(a), reinterpret_cast<byte_t const*>(b));
    expect(std::isfinite(d_full) && std::isfinite(d_ser), tag);
    expect(close_f(d_full, d_ser, 1e-4f, 1e-4f), tag);
}

void test_diff_dims() {
    std::mt19937 rng(42);
    std::size_t dims[] = {16, 64, 256, 1536};
    metric_kind_t kinds[] = {metric_kind_t::cos_k, metric_kind_t::ip_k, metric_kind_t::l2sq_k};

    for (std::size_t dim : dims) {
        std::vector<float> a(dim), b(dim), near_v(dim), anti_v(dim);
        fill_random(a, rng);
        fill_random(b, rng);
        fill_near_dup(a, near_v, 1e-3f, rng);
        fill_antipodal(a, anti_v);

        for (metric_kind_t kind : kinds) {
            compare_pair(dim, kind, a.data(), b.data(), "random");
            compare_pair(dim, kind, a.data(), near_v.data(), "near_dup");
            compare_pair(dim, kind, a.data(), anti_v.data(), "antipodal");
        }
    }
}

void test_diff_i8() {
    constexpr std::size_t dim = 64;
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> dist(-127, 127);
    std::vector<i8_t> a(dim), b(dim);
    for (std::size_t i = 0; i < dim; ++i) {
        a[i] = static_cast<i8_t>(dist(rng));
        b[i] = static_cast<i8_t>(dist(rng));
    }

    metric_kind_t kinds[] = {metric_kind_t::cos_k, metric_kind_t::ip_k, metric_kind_t::l2sq_k};
    for (metric_kind_t kind : kinds) {
        auto m_full = metric_punned_t::builtin(dim, kind, scalar_kind_t::i8_k);
        auto m_serial = metric_punned_t::builtin_with_caps(dim, kind, scalar_kind_t::i8_k, nk_cap_serial_k);
        expect(bool(m_full) && bool(m_serial), "i8 metrics");
        float d_full = m_full(reinterpret_cast<byte_t const*>(a.data()), reinterpret_cast<byte_t const*>(b.data()));
        float d_ser = m_serial(reinterpret_cast<byte_t const*>(a.data()), reinterpret_cast<byte_t const*>(b.data()));
        expect(std::isfinite(d_full) && std::isfinite(d_ser), "i8 finite");
        // i8 kernels may use wider accumulators; allow slightly looser tol than f32.
        expect(close_f(d_full, d_ser, 1e-3f, 1e-3f), "i8 numkong vs serial");
    }
}

void test_diff_bf16() {
    constexpr std::size_t dim = 64;
    std::mt19937 rng(13);
    std::vector<float> af(dim), bf(dim);
    fill_random(af, rng);
    fill_random(bf, rng);
    std::vector<bf16_t> a(dim), b(dim);
    for (std::size_t i = 0; i < dim; ++i) {
        a[i] = bf16_t(af[i]);
        b[i] = bf16_t(bf[i]);
    }

    metric_kind_t kinds[] = {metric_kind_t::cos_k, metric_kind_t::ip_k, metric_kind_t::l2sq_k};
    for (metric_kind_t kind : kinds) {
        auto m_full = metric_punned_t::builtin(dim, kind, scalar_kind_t::bf16_k);
        auto m_serial = metric_punned_t::builtin_with_caps(dim, kind, scalar_kind_t::bf16_k, nk_cap_serial_k);
        expect(bool(m_full) && bool(m_serial), "bf16 metrics");
        float d_full = m_full(reinterpret_cast<byte_t const*>(a.data()), reinterpret_cast<byte_t const*>(b.data()));
        float d_ser = m_serial(reinterpret_cast<byte_t const*>(a.data()), reinterpret_cast<byte_t const*>(b.data()));
        expect(std::isfinite(d_full) && std::isfinite(d_ser), "bf16 finite");
        expect(close_f(d_full, d_ser, 2e-3f, 2e-3f), "bf16 numkong vs serial");
    }
}

void test_topk_order() {
    constexpr std::size_t dim = 64;
    constexpr std::size_t n = 32;
    constexpr std::size_t k = 5;
    std::mt19937 rng(7);
    std::vector<float> data(n * dim), query(dim);
    fill_random(query, rng);
    {
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        for (float& x : data)
            x = dist(rng);
    }

    auto m_full = metric_punned_t::builtin(dim, metric_kind_t::cos_k, scalar_kind_t::f32_k);
    auto m_serial = metric_punned_t::builtin_with_caps(dim, metric_kind_t::cos_k, scalar_kind_t::f32_k, nk_cap_serial_k);

    std::vector<std::pair<float, std::size_t>> full_scores, ser_scores;
    full_scores.reserve(n);
    ser_scores.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        float const* row = data.data() + i * dim;
        full_scores.emplace_back(
            m_full(reinterpret_cast<byte_t const*>(query.data()), reinterpret_cast<byte_t const*>(row)), i);
        ser_scores.emplace_back(
            m_serial(reinterpret_cast<byte_t const*>(query.data()), reinterpret_cast<byte_t const*>(row)), i);
    }
    auto by_dist = [](auto const& x, auto const& y) { return x.first < y.first; };
    std::partial_sort(full_scores.begin(), full_scores.begin() + static_cast<std::ptrdiff_t>(k), full_scores.end(),
                      by_dist);
    std::partial_sort(ser_scores.begin(), ser_scores.begin() + static_cast<std::ptrdiff_t>(k), ser_scores.end(),
                      by_dist);
    for (std::size_t i = 0; i < k; ++i)
        expect(full_scores[i].second == ser_scores[i].second, "topk index match");
}

bool host_has_simd_beyond_serial() {
    nk_capability_t avail = nk_capabilities_available();
    return (avail & ~nk_cap_serial_k) != 0;
}

void test_l4_nails() {
    // ip = 1 - dot (NumKong path): identical unit vectors → ip distance ~ 0.
    float u[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    auto m_ip = metric_punned_t::builtin(4, metric_kind_t::ip_k, scalar_kind_t::f32_k);
    float dip = m_ip(reinterpret_cast<byte_t const*>(u), reinterpret_cast<byte_t const*>(u));
    expect(close_f(dip, 0.0f, 1e-5f, 0.0f), "ip self ~ 0 (1-dot)");

    // Binary Hamming: dimensions are bits, not bytes.
    constexpr std::size_t bits = 64;
    std::uint8_t ba[8] = {0xFF, 0, 0, 0, 0, 0, 0, 0};
    std::uint8_t bb[8] = {0x0F, 0, 0, 0, 0, 0, 0, 0};
    auto m_ham = metric_punned_t::builtin(bits, metric_kind_t::hamming_k, scalar_kind_t::b1x8_k);
    float dh = m_ham(reinterpret_cast<byte_t const*>(ba), reinterpret_cast<byte_t const*>(bb));
    expect(close_f(dh, 4.0f, 1e-5f, 0.0f), "hamming bit-dim (4 differing bits in low nibble)");

    // Sorensen must NOT route through Jaccard NumKong path.
    auto m_sor = metric_punned_t::builtin(bits, metric_kind_t::sorensen_k, scalar_kind_t::b1x8_k);
    expect(bool(m_sor), "sorensen configures");
    expect(std::strcmp(m_sor.isa_name(), "serial") == 0, "sorensen must not use NumKong jaccard");

    char const* accel = hardware_acceleration_available();
    expect(accel && accel[0], "hardware_acceleration_available non-empty");

    // On runners with SIMD compiled+present, the available string must list more than serial.
    if (host_has_simd_beyond_serial()) {
        std::string s(accel);
        expect(s.find("serial") != std::string::npos, "available lists serial");
        expect(s != "serial", "available must include a non-serial ISA on SIMD hosts");
        auto m_cos = metric_punned_t::builtin(64, metric_kind_t::cos_k, scalar_kind_t::f32_k);
        expect(std::strcmp(m_cos.isa_name(), "serial") != 0, "f32 cos should pick SIMD when available");
    }

    char const* compiled = hardware_acceleration_compiled();
    expect(compiled && compiled[0], "hardware_acceleration_compiled non-empty");
}

} // namespace

int main() {
    (void)nk_capabilities();
    test_diff_dims();
    test_diff_i8();
    test_diff_bf16();
    test_topk_order();
    test_l4_nails();

    if (g_fails) {
        std::fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    std::puts("test_numkong_diff ok");
    return 0;
}
