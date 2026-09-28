/**
 *  @file       fuzz_format.cpp
 *  @brief      libFuzzer：任意字节 → dense 文件头/metadata 解析（不 load 整图，防恶意维度 OOM）。
 *
 *  Build: -DUSEARCH_BUILD_FUZZ=ON
 *  Run:   ./fuzz_format -max_total_time=60
 */

#include <cstdint>
#include <cstring>

#include <dense/dense.hpp>

using namespace unum::usearch;

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
    // 空/过大输入无收益；metadata 只需前缀，上限挡住无意义膨胀
    if (size == 0 || size > (1u << 20))
        return 0;

    // 非拥有包装：path_ 为空，析构不会 munmap/free 调用方缓冲
    memory_mapped_file_t view(const_cast<byte_t*>(reinterpret_cast<byte_t const*>(data)), size);
    auto meta = index_dense_metadata_from_buffer(view);
    if (meta) {
        // 读几个字段，迫使 sanitizer 覆盖 misaligned_ref 路径
        volatile auto dims = static_cast<std::uint64_t>(meta.head.dimensions);
        volatile auto metric = static_cast<std::uint8_t>(meta.head.kind_metric);
        volatile auto scalar = static_cast<std::uint8_t>(meta.head.kind_scalar);
        (void)dims;
        (void)metric;
        (void)scalar;
    }

    // 合成「魔数开头」的变体：前 7 字节改成 usearch，再解析一次（覆盖 exclude_vectors 捷径）
    if (size >= sizeof(index_dense_head_buffer_t)) {
        alignas(8) byte_t buf[sizeof(index_dense_head_buffer_t)];
        std::memcpy(buf, data, sizeof(buf));
        std::memcpy(buf, default_magic(), std::strlen(default_magic()));
        memory_mapped_file_t magic_view(buf, sizeof(buf));
        auto m2 = index_dense_metadata_from_buffer(magic_view);
        if (m2) {
            volatile auto maj = static_cast<std::uint16_t>(m2.head.version_major);
            (void)maj;
        }
    }

    return 0;
}
