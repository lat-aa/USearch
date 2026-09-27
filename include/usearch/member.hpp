/**
 *  @file       member.hpp
 *  @brief      序列化头与 member_gt/ref/cref 键槽视图。
 */
#pragma once
#include <usearch/io.hpp>
namespace unum {
namespace usearch {

/**
 *  @brief  Metadata header for the serialized index.
 *
 *  This structure is very minimalistic by design. It contains no information
 *  about the capacity of the index, so you'll have to `reserve` after loading.
 *  It also contains no info on the metric or key types, so you'll have to store
 *  that information elsewhere, like we do in `index_dense_head_t`.
 */
struct index_serialized_header_t {
    std::uint64_t size = 0;
    std::uint64_t connectivity = 0;
    std::uint64_t connectivity_base = 0;
    std::uint64_t max_level = 0;
    std::uint64_t entry_slot = 0;
};

using default_key_t = std::uint64_t;
using default_slot_t = std::uint32_t;
using default_distance_t = float;

template <typename key_at = default_key_t> struct member_gt {
    key_at key;
    std::size_t slot;
};

template <typename key_at> inline std::size_t get_slot(member_gt<key_at> const& m) noexcept { return m.slot; }
template <typename key_at> inline key_at get_key(member_gt<key_at> const& m) noexcept { return m.key; }

template <typename key_at = default_key_t> struct member_cref_gt {
    misaligned_ref_gt<key_at const> key;
    std::size_t slot;
};

template <typename key_at> inline std::size_t get_slot(member_cref_gt<key_at> const& m) noexcept { return m.slot; }
template <typename key_at> inline key_at get_key(member_cref_gt<key_at> const& m) noexcept { return m.key; }

template <typename key_at = default_key_t> struct member_ref_gt {
    misaligned_ref_gt<key_at> key;
    std::size_t slot;

    inline operator member_cref_gt<key_at>() const noexcept { return {key.ptr(), slot}; }
};

template <typename key_at> inline std::size_t get_slot(member_ref_gt<key_at> const& m) noexcept { return m.slot; }
template <typename key_at> inline key_at get_key(member_ref_gt<key_at> const& m) noexcept { return m.key; }

/**
 *  @brief  Approximate Nearest Neighbors Search @b index-structure using the
 *          Hierarchical Navigable Small World @b (HNSW) graphs algorithm.
 *          If classical containers store @b Key->Value mappings, this one can
 *          be seen as a network of keys, accelerating approximate @b Value~>Key visited_members.
 *
 *  Unlike most implementations, this one is generic and can be used for any search,
 *  not just within equi-dimensional vectors. Examples range from Texts to similar Chess
 *  positions, Geo-Spatial Search, and even Graphs.
 *
 *  @tparam key_at
 *      The type of primary objects stored in the index.
 *      The values, to which those map, are not managed by the same index structure.
 *
 *  @tparam compressed_slot_at
 *      The smallest unsigned integer type to address indexed elements.
 *      It is used internally to maximize space-efficiency and is generally
 *      up-casted to @b `std::size_t` in public interfaces.
 *      Can be a built-in @b `uint32_t`, `uint64_t`, or our custom @b `uint40_t`.
 *      Which makes the most sense for 4B+ entry indexes.
 *
 *  @tparam dynamic_allocator_at
 *      Dynamic memory allocator for temporary buffers, visits indicators, and
 *      priority queues, needed during construction and traversals of graphs.
 *      The allocated buffers may be uninitialized.
 *
 *  @tparam tape_allocator_at
 *      Potentially different memory allocator for primary allocations of nodes and vectors.
 *      It would never `deallocate` separate entries, and would only free all the space at once.
 *      The allocated buffers may be uninitialized.
 *
 *  @section Features
 *
 *      - Thread-safe for concurrent construction, search, and updates.
 *      - Doesn't allocate new threads, and reuses the ones its called from.
 *      - Allows storing value externally, managing just the similarity index.
 *      - Joins.

 *  @section Usage
 *
 *  @subsection Exceptions
 *
 *  None of the methods throw exceptions in the "Release" compilation mode.
 *  It may only `throw` if your memory ::dynamic_allocator_at or ::metric_at isn't
 *  safe to copy.
 *
 *  @subsection Serialization
 *
 *  When serialized, doesn't include any additional metadata.
 *  It is just the multi-level proximity-graph. You may want to store metadata about
 *  the used metric and key types somewhere else.
 *
 *  @section Implementation Details
 *
 *  Like every HNSW implementation, USearch builds levels of "Proximity Graphs".
 *  Every added vector forms a node in one or more levels of the graph.
 *  Every node is present in the base level. Every following level contains a smaller
 *  fraction of nodes. During search, the operation starts with the smaller levels
 *  and zooms-in on every following iteration of larger graph traversals.
 *
 *  Just one memory allocation is performed regardless of the number of levels.
 *  The adjacency lists across all levels are concatenated into that single buffer.
 *  That buffer starts with a "head", that stores the metadata, such as the
 *  tallest "level" of the graph that it belongs to, the external "key", and the
 *  number of "dimensions" in the vector.
 *
 *  @section Metrics, Predicates and Callbacks
 *
 *
 *  @section Smart References and Iterators
 *
 *      -   `member_citerator_t` and `member_iterator_t` have only slots, no indirections.
 *
 *      -   `member_cref_t` and `member_ref_t` contains the `slot` and a reference
 *          to the key. So it passes through 1 level of visited_members in `nodes_`.
 *          Retrieving the key via `get_key` will cause fetching yet another cache line.
 *
 *      -   `member_gt` contains an already prefetched copy of the key.
 *
 */

} // namespace usearch
} // namespace unum
