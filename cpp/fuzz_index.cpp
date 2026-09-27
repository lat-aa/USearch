/**
 *  @file       fuzz_index.cpp
 *  @brief      libFuzzer harness：变异向量 → add/search/save/load 往返（不加载不可信文件头）。
 *
 *  Build: -DUSEARCH_BUILD_FUZZ=ON (Clang with libFuzzer).
 *  Run:   ./fuzz_index -max_total_time=60
 */

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <usearch/index_dense.hpp>

namespace fs = std::filesystem;
using namespace unum::usearch;

using dense_t = index_dense_gt<>;

static fs::path tempPath(std::uint64_t tag) {
    auto dir = fs::temp_directory_path() / "usearch_fuzz";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / ("c" + std::to_string(tag) + ".usearch");
}

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
    // 过小无覆盖；过大浪费。不把任意字节当 .usearch 头解析（会 OOM）。
    if (size < 8 || size > 1 << 16)
        return 0;

    std::uint64_t tag = 0;
    std::memcpy(&tag, data, sizeof(std::uint32_t));

    constexpr std::size_t dim = 8;
    auto made = dense_t::make(metric_punned_t(dim, metric_kind_t::cos_k));
    if (!made)
        return 0;

    dense_t index = std::move(made.index);
    (void)index.try_reserve(index_limits_t(64));

    float v[dim] = {};
    std::size_t n = (std::min)(size / sizeof(float), static_cast<std::size_t>(32));
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t d = 0; d < dim; ++d) {
            std::uint8_t b = data[(i * dim + d) % size];
            v[d] = static_cast<float>((b % 31) + 1);
        }
        (void)index.add(static_cast<default_key_t>(i + 1), v);
    }

    float q[dim] = {};
    for (std::size_t d = 0; d < dim; ++d)
        q[d] = static_cast<float>((data[d % size] % 17) + 1);
    auto results = index.search(q, 4);
    if (results) {
        std::vector<default_key_t> keys(4);
        std::vector<float> distances(4);
        (void)results.dump_to(keys.data(), distances.data());
    }

    fs::path path = tempPath(tag);
    if (index.save(path.string().c_str())) {
        auto loaded = dense_t::make(metric_punned_t(dim, metric_kind_t::cos_k));
        if (loaded) {
            dense_t other = std::move(loaded.index);
            // 仅加载本 harness 写出的合法文件。
            if (other.load(path.string().c_str()))
                (void)other.search(q, 2);
            if (other.view(path.string().c_str()))
                (void)other.search(q, 2);
        }
    }

    std::error_code ec;
    fs::remove(path, ec);
    return 0;
}
