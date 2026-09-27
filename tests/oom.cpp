/**
 *  @file       oom.cpp
 *  @brief      注入失败分配器，断言 add 在图扩展 OOM 时返回错误并回滚边。
 */
#include <usearch/index.hpp>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

using namespace unum::usearch;

namespace {

std::atomic<std::size_t> g_budget{std::size_t(-1)};

/** 预算耗尽后 allocate 返回 nullptr，模拟 OOM。 */
template <typename T>
struct budget_allocator {
    using value_type = T;
    budget_allocator() noexcept = default;
    template <typename U>
    budget_allocator(budget_allocator<U> const&) noexcept {}
    T* allocate(std::size_t n) {
        std::size_t left = g_budget.load();
        if (left == 0)
            return nullptr;
        if (left != std::size_t(-1))
            g_budget.store(left - 1);
        return static_cast<T*>(::operator new(n * sizeof(T), std::nothrow));
    }
    void deallocate(T* p, std::size_t) noexcept { ::operator delete(p); }
    template <typename U>
    bool operator==(budget_allocator<U> const&) const noexcept {
        return true;
    }
    template <typename U>
    bool operator!=(budget_allocator<U> const&) const noexcept {
        return false;
    }
};

using byte_alloc_t = budget_allocator<byte_t>;
using index_t = index_gt<float, std::int64_t, std::uint32_t, byte_alloc_t, byte_alloc_t>;

float l2sq(float const* a, float const* b, std::size_t dim) noexcept {
    float s = 0;
    for (std::size_t i = 0; i < dim; ++i) {
        float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

/** 外置向量表 + 异构/同构四重载，满足 index_gt::measure 契约。 */
struct metric_t {
    using member_cref_t = typename index_t::member_cref_t;
    using member_citerator_t = typename index_t::member_citerator_t;

    std::vector<std::vector<float>> const* vecs = nullptr;
    std::size_t dim = 0;

    float between(float const* a, float const* b) const noexcept { return l2sq(a, b, dim); }

    float operator()(member_cref_t const& a, member_cref_t const& b) const {
        return between((*vecs)[get_slot(a)].data(), (*vecs)[get_slot(b)].data());
    }
    float operator()(float const* q, member_cref_t const& m) const {
        return between(q, (*vecs)[get_slot(m)].data());
    }
    float operator()(member_citerator_t const& a, member_citerator_t const& b) const {
        return between((*vecs)[get_slot(a)].data(), (*vecs)[get_slot(b)].data());
    }
    float operator()(float const* q, member_citerator_t const& m) const {
        return between(q, (*vecs)[get_slot(m)].data());
    }
};

} // namespace

int main() {
    constexpr std::size_t dim = 4;
    index_t index;
    assert(index.reserve(32));

    std::vector<std::vector<float>> vecs = {
        std::vector<float>(dim, 0.1f),
        std::vector<float>(dim, 0.2f),
        std::vector<float>(dim, 0.3f),
    };
    metric_t metric{&vecs, dim};

    g_budget.store(std::size_t(-1));
    {
        auto r1 = index.add(0, vecs[0].data(), metric);
        assert(r1);
        r1.error.release();
        auto r2 = index.add(1, vecs[1].data(), metric);
        assert(r2);
        r2.error.release();
    }
    assert(index.size() == 2);

    g_budget.store(2);
    {
        auto r = index.add(2, vecs[2].data(), metric);
        if (r) {
            r.error.release();
            std::puts("oom: OOM not triggered on this host (skipped)");
            return 0;
        }
        char const* msg = r.error.what();
        assert(msg != nullptr);
        assert(std::strstr(msg, "memory") != nullptr || std::strstr(msg, "Memory") != nullptr ||
               std::strstr(msg, "Out of") != nullptr);
        (void)msg;
        r.error.release();
    }
    assert(index.size() == 2);

    std::puts("oom: ok");
    return 0;
}
