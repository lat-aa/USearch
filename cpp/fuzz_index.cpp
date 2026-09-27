/**
 *  @file       fuzz_index.cpp
 *  @brief      libFuzzer harness: mutate bytes → temp .usearch → load/view/search.
 *
 *  Build: -DUSEARCH_BUILD_FUZZ=ON (Clang with libFuzzer).
 *  Run:   ./fuzz_index -max_total_time=60
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    if (size < 4 || size > 1 << 20)
        return 0;

    std::uint64_t tag = 0;
    std::memcpy(&tag, data, sizeof(std::uint32_t));
    fs::path path = tempPath(tag);

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return 0;
        out.write(reinterpret_cast<char const*>(data), static_cast<std::streamsize>(size));
    }

    // 损坏文件必须失败软：不得 abort / 泄漏到 sanitizer 之外的崩溃。
    {
        auto made = dense_t::make(metric_punned_t(16, metric_kind_t::cos_k));
        if (made) {
            dense_t index = std::move(made.index);
            (void)index.load(path.string().c_str());
            (void)index.view(path.string().c_str());
            float q[16] = {};
            auto results = index.search(q, 4);
            if (results) {
                std::vector<default_key_t> keys(4);
                std::vector<float> distances(4);
                (void)results.dump_to(keys.data(), distances.data());
            }
        }
    }

    // 合法小索引：写入再加载，覆盖序列化往返。
    if (size >= 64) {
        auto made = dense_t::make(metric_punned_t(8, metric_kind_t::cos_k));
        if (made) {
            dense_t index = std::move(made.index);
            (void)index.try_reserve(index_limits_t(32));
            float v[8] = {1, 0, 0, 0, 0, 0, 0, 0};
            for (std::uint64_t i = 0; i < 4; ++i) {
                v[0] = static_cast<float>((data[i % size] % 17) + 1);
                (void)index.add(i + 1, v);
            }
            fs::path good = tempPath(tag ^ 0x9e3779b97f4a7c15ull);
            if (index.save(good.string().c_str())) {
                auto loaded = dense_t::make(metric_punned_t(8, metric_kind_t::cos_k));
                if (loaded) {
                    dense_t other = std::move(loaded.index);
                    (void)other.load(good.string().c_str());
                    (void)other.search(v, 2);
                }
            }
            std::error_code ec;
            fs::remove(good, ec);
        }
    }

    std::error_code ec;
    fs::remove(path, ec);
    return 0;
}
