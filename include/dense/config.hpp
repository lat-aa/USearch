/**
 *  @file       config.hpp
 *  @brief      稠密索引构造/序列化/拷贝/聚类配置。
 */
#pragma once
#include <usearch/index.hpp>
#include <plugins/plugins.hpp>
namespace unum {
namespace usearch {

struct index_dense_config_t : public index_config_t {
    std::size_t expansion_add = default_expansion_add();
    std::size_t expansion_search = default_expansion_search();

    /**
     *  @brief  Excludes vectors from the serialized file.
     *          This is handy when you want to store the vectors in a separate file.
     *
     *  ! For advanced users only.
     */
    bool exclude_vectors = false;

    /**
     *  @brief  Allows you to store multiple vectors per key.
     *          This is handy when a large document is chunked into many parts.
     *
     *  ! May degrade the performance of iterators.
     */
    bool multi = false;

    /**
     *  @brief  Allows you to reduce RAM consumption by avoiding
     *          reverse-indexing keys-to-vectors, and only keeping
     *          the vectors-to-keys mappings.
     *
     *  ! This configuration parameter doesn't affect the serialized file,
     *  ! and is not preserved between runs. Makes sense for smaller vectors
     *  ! that fit in a couple of cache lines.
     *
     *  The trade-off is that some methods won't be available, like `get`, `rename`,
     *  and `remove`. The basic functionality, like `add` and `search` will work as
     *  expected even with `enable_key_lookups = false`.
     *
     *  If both `!multi && !enable_key_lookups`, the "duplicate entry" checks won't
     *  be performed and no errors will be raised.
     */
    bool enable_key_lookups = true;

    inline index_dense_config_t(index_config_t base) noexcept : index_config_t(base) {}

    inline index_dense_config_t(std::size_t c = 0, std::size_t ea = 0, std::size_t es = 0) noexcept
        : index_config_t(c), expansion_add(ea), expansion_search(es) {}

    /**
     *  @brief  Validates the configuration settings, updating them in-place.
     *  @return Error message, if any.
     */
    inline error_t validate() noexcept {
        error_t error = index_config_t::validate();
        if (error)
            return error;
        if (expansion_add == 0)
            expansion_add = default_expansion_add();
        if (expansion_search == 0)
            expansion_search = default_expansion_search();
        return {};
    }
};

struct index_dense_clustering_config_t {
    std::size_t min_clusters = 0;
    std::size_t max_clusters = 0;
    enum mode_t {
        merge_smallest_k,
        merge_closest_k,
    } mode = merge_smallest_k;
};

struct index_dense_serialization_config_t {
    bool exclude_vectors = false;
    bool use_64_bit_dimensions = false;
};

struct index_dense_copy_config_t : public index_copy_config_t {
    bool force_vector_copy = true;

    index_dense_copy_config_t() = default;
    index_dense_copy_config_t(index_copy_config_t base) noexcept : index_copy_config_t(base) {}
};

} // namespace usearch
} // namespace unum
