/**
 *  @file       config.hpp
 *  @brief      索引构造/检索/聚类/join 配置与默认超参。
 */
#pragma once
#include <usearch/heap.hpp>
namespace unum {
namespace usearch {

/// @brief Number of neighbors per graph node.
/// Defaults to 32 in FAISS and 16 in hnswlib.
/// > It is called `M` in the paper.
constexpr std::size_t default_connectivity() { return 16; }

/// @brief Hyper-parameter controlling the quality of indexing.
/// Defaults to 40 in FAISS and 200 in hnswlib.
/// > It is called `efConstruction` in the paper.
constexpr std::size_t default_expansion_add() { return 128; }

/// @brief Hyper-parameter controlling the quality of search.
/// Defaults to 16 in FAISS and 10 in hnswlib.
/// > It is called `ef` in the paper.
constexpr std::size_t default_expansion_search() { return 64; }

constexpr std::size_t default_allocator_entry_bytes() { return 64; }

/**
 *  @brief  Configuration settings for the index construction.
 *          Includes the main `::connectivity` parameter (`M` in the paper)
 *          and two expansion factors - for construction and search.
 */
struct index_config_t {
    /// @brief Number of neighbors per graph node.
    /// Defaults to 32 in FAISS and 16 in hnswlib.
    /// > It is called `M` in the paper.
    std::size_t connectivity = default_connectivity();

    /// @brief Number of neighbors per graph node in base level graph.
    /// Defaults to double of the other levels, so 64 in FAISS and 32 in hnswlib.
    /// > It is called `M0` in the paper.
    std::size_t connectivity_base = default_connectivity() * 2;

    inline index_config_t() = default;
    inline index_config_t(std::size_t c, std::size_t cb = 0) noexcept : connectivity(c), connectivity_base(cb) {}

    /**
     *  @brief  Validates the configuration settings, updating them in-place.
     *  @return Error message, if any.
     */
    inline error_t validate() noexcept {
        if (connectivity == 0)
            connectivity = default_connectivity();
        if (connectivity_base == 0) {
            checked_size_result_t default_base = checked_mul(connectivity, std::size_t{2});
            if (!default_base)
                return "Connectivity is too large";
            connectivity_base = default_base.value;
        }
        if (connectivity < 2)
            return "Connectivity must be at least 2, otherwise the index degenerates into ropes";
        if (connectivity_base < connectivity)
            return "Base layer should be at least as connected as the rest of the graph";
        checked_size_result_t neighbors_bytes =
            checked_mul_add(connectivity, sizeof(std::uint64_t), sizeof(std::uint32_t));
        checked_size_result_t neighbors_base_bytes =
            checked_mul_add(connectivity_base, sizeof(std::uint64_t), sizeof(std::uint32_t));
        if (!neighbors_bytes || !neighbors_base_bytes)
            return "Connectivity is too large";
        return {};
    }

    /**
     *  @brief  Immutable function to check if the configuration is valid.
     *  @return `true` if the configuration is valid.
     */
    inline bool is_valid() const noexcept { return connectivity >= 2 && connectivity_base >= connectivity; }
};

/**
 *  @brief  Tag type selecting the "no upfront reservation" overload of
 *          @ref index_limits_t.  Modeled after @c std::defer_lock: the
 *          resulting limits are all-zero and produce no allocations when
 *          handed to @ref index_dense_gt::try_reserve.
 */
struct unreserved_t {};
constexpr unreserved_t unreserved{};

/**
 *  @brief  Growth settings for the index container.
 *          Includes the upper bound for `::members` capacity,
 *          and the number of read/write threads expected to work with the index.
 */
struct index_limits_t {
    /// @brief Maximum number of entries in the index.
    std::size_t members;
    /// @brief Max number of threads simultaneously updating entries.
    std::size_t threads_add;
    /// @brief Max number of threads simultaneously searching entries.
    std::size_t threads_search;

    inline index_limits_t(std::size_t n, std::size_t t) noexcept : members(n), threads_add(t), threads_search(t) {}
    inline index_limits_t(std::size_t n = 0) noexcept
        : index_limits_t(n, (std::max<std::size_t>)(1, std::thread::hardware_concurrency())) {}
    inline index_limits_t(unreserved_t) noexcept : members(0), threads_add(0), threads_search(0) {}
    /// @brief Returns the upper limit for the number of threads.
    inline std::size_t threads() const noexcept { return (std::max)(threads_add, threads_search); }
    /// @brief Returns the concurrency-level of the index - the minimum of thread counts.
    inline std::size_t concurrency() const noexcept { return (std::min)(threads_add, threads_search); }
    /// @brief Returns a copy with zero thread counts replaced by the library default.
    ///        Use when carrying limits forward across operations that may have left
    ///        @c threads_add / @c threads_search unset (e.g. @c unreserved construction).
    inline index_limits_t with_thread_defaults() const noexcept {
        index_limits_t result = *this;
        index_limits_t const defaults;
        if (!result.threads_add)
            result.threads_add = defaults.threads_add;
        if (!result.threads_search)
            result.threads_search = defaults.threads_search;
        return result;
    }
};

struct index_update_config_t {
    /// @brief Hyper-parameter controlling the quality of indexing.
    /// Defaults to 40 in FAISS and 200 in hnswlib.
    /// > It is called `efConstruction` in the paper.
    std::size_t expansion = default_expansion_add();

    /// @brief Optional thread identifier for multi-threaded construction.
    std::size_t thread = 0;
};

struct index_search_config_t {
    /// @brief Hyper-parameter controlling the quality of search.
    /// Defaults to 16 in FAISS and 10 in hnswlib.
    /// > It is called `ef` in the paper.
    std::size_t expansion = default_expansion_search();

    /// @brief Optional thread identifier for multi-threaded construction.
    std::size_t thread = 0;

    /// @brief Brute-forces exhaustive search over all entries in the index.
    bool exact = false;
};

struct index_cluster_config_t {
    /// @brief Hyper-parameter controlling the quality of search.
    /// Defaults to 16 in FAISS and 10 in hnswlib.
    /// > It is called `ef` in the paper.
    std::size_t expansion = default_expansion_search();

    /// @brief Optional thread identifier for multi-threaded construction.
    std::size_t thread = 0;
};

struct index_copy_config_t {};

struct index_join_config_t {
    /// @brief Controls maximum number of proposals per man during stable marriage.
    std::size_t max_proposals = 0;

    /// @brief Hyper-parameter controlling the quality of search.
    /// Defaults to 16 in FAISS and 10 in hnswlib.
    /// > It is called `ef` in the paper.
    std::size_t expansion = default_expansion_search();

    /// @brief Brute-forces exhaustive search over all entries in the index.
    bool exact = false;
};

} // namespace usearch
} // namespace unum
