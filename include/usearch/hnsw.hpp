/**
 *  @file       hnsw.hpp
 *  @brief      index_gt（HNSW）与自由函数 join；只依赖 usearch 图栈，不含 plugins/dense。
 */
#pragma once
#include <usearch/member.hpp>
namespace unum {
namespace usearch {

template <typename distance_at = default_distance_t,              //
          typename key_at = default_key_t,                        //
          typename compressed_slot_at = default_slot_t,           //
          typename dynamic_allocator_at = std::allocator<byte_t>, //
          typename tape_allocator_at = dynamic_allocator_at>      //
class index_gt {
  public:
    using distance_t = distance_at;
    using vector_key_t = key_at;
    using key_t = vector_key_t;
    using compressed_slot_t = compressed_slot_at;
    using dynamic_allocator_t = dynamic_allocator_at;
    using tape_allocator_t = tape_allocator_at;
    static_assert(sizeof(vector_key_t) >= sizeof(compressed_slot_t), "Having tiny keys doesn't make sense.");
    static_assert(std::is_signed<distance_t>::value, "Distance must be a signed type, as we use the unary minus.");

    using member_cref_t = member_cref_gt<vector_key_t>;
    using member_ref_t = member_ref_gt<vector_key_t>;

    template <typename ref_at, typename index_at> class member_iterator_gt {
        using ref_t = ref_at;
        using index_t = index_at;

        friend class index_gt;
        member_iterator_gt() noexcept {}
        member_iterator_gt(index_t* index, compressed_slot_t slot) noexcept : index_(index), slot_(slot) {}

        template <int> ref_t call_key(std::true_type) const noexcept {
            return ref_t{index_->node_at_(slot_).ckey(), slot_};
        }
        template <int> ref_t call_key(std::false_type) const noexcept {
            return ref_t{index_->node_at_(slot_).key(), slot_};
        }

        index_t* index_{};
        compressed_slot_t slot_{};

      public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = ref_t;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = ref_t;

        reference operator*() const noexcept { return call_key<0>(std::is_const<index_t>()); }
        vector_key_t key() const noexcept { return index_->node_at_(slot_).ckey(); }

        friend inline compressed_slot_t get_slot(member_iterator_gt const& it) noexcept { return it.slot_; }
        friend inline vector_key_t get_key(member_iterator_gt const& it) noexcept { return it.key(); }

        // clang-format off
        member_iterator_gt operator++(int) noexcept { member_iterator_gt old(index_, slot_); ++(*this); return old; }
        member_iterator_gt operator--(int) noexcept { member_iterator_gt old(index_, slot_); --(*this); return old; }
        member_iterator_gt operator+(difference_type d) noexcept { return member_iterator_gt(index_, static_cast<compressed_slot_t>(static_cast<std::size_t>(slot_) + d)); }
        member_iterator_gt operator-(difference_type d) noexcept { return member_iterator_gt(index_, static_cast<compressed_slot_t>(static_cast<std::size_t>(slot_) - d)); }
        member_iterator_gt& operator++() noexcept { slot_ = static_cast<compressed_slot_t>(static_cast<std::size_t>(slot_) + 1); return *this; }
        member_iterator_gt& operator--() noexcept { slot_ = static_cast<compressed_slot_t>(static_cast<std::size_t>(slot_) - 1); return *this; }
        member_iterator_gt& operator+=(difference_type d) noexcept { slot_ = static_cast<compressed_slot_t>(static_cast<std::size_t>(slot_) + d); return *this; }
        member_iterator_gt& operator-=(difference_type d) noexcept { slot_ = static_cast<compressed_slot_t>(static_cast<std::size_t>(slot_) - d); return *this; }
        bool operator==(member_iterator_gt const& other) const noexcept { return index_ == other.index_ && slot_ == other.slot_; }
        bool operator!=(member_iterator_gt const& other) const noexcept { return index_ != other.index_ || slot_ != other.slot_; }
        // clang-format on
    };

    using member_iterator_t = member_iterator_gt<member_ref_t, index_gt>;
    using member_citerator_t = member_iterator_gt<member_cref_t, index_gt const>;

    // STL compatibility:
    using value_type = vector_key_t;
    using allocator_type = dynamic_allocator_t;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = member_ref_t;
    using const_reference = member_cref_t;
    using pointer = void;
    using const_pointer = void;
    using iterator = member_iterator_t;
    using const_iterator = member_citerator_t;
    using reverse_iterator = std::reverse_iterator<member_iterator_t>;
    using reverse_const_iterator = std::reverse_iterator<member_citerator_t>;

    using dynamic_allocator_traits_t = std::allocator_traits<dynamic_allocator_t>;
    using byte_t = typename dynamic_allocator_t::value_type;
    static_assert(           //
        sizeof(byte_t) == 1, //
        "Primary allocator must allocate separate addressable bytes");

    using tape_allocator_traits_t = std::allocator_traits<tape_allocator_t>;
    static_assert(                                                 //
        sizeof(typename tape_allocator_traits_t::value_type) == 1, //
        "Tape allocator must allocate separate addressable bytes");

  private:
    /**
     *  @brief  Integer for the number of node neighbors at a specific level of the
     *          multi-level graph. It's selected to be `std::uint32_t` to improve the
     *          alignment in most common cases.
     */
    using neighbors_count_t = std::uint32_t;
    using level_t = std::int16_t;

    /**
     *  @brief  How many bytes of memory are needed to form the "head" of the node.
     */
    static constexpr std::size_t node_head_bytes_() { return sizeof(vector_key_t) + sizeof(level_t); }

    using nodes_mutexes_t = striped_locks_gt<dynamic_allocator_t>;

    using visits_hash_set_t = growing_hash_set_gt<compressed_slot_t, hash_gt<compressed_slot_t>, dynamic_allocator_t>;

    struct precomputed_constants_t {
        double inverse_log_connectivity{};
        std::size_t neighbors_bytes{};
        std::size_t neighbors_base_bytes{};
    };
    /// @brief A space-efficient internal data-structure used in graph traversal queues.
    struct candidate_t {
        distance_t distance;
        compressed_slot_t slot;
        inline bool operator<(candidate_t other) const noexcept { return distance < other.distance; }
    };

    using candidates_view_t = span_gt<candidate_t const>;
    using candidates_allocator_t = typename dynamic_allocator_traits_t::template rebind_alloc<candidate_t>;
    using top_candidates_t = sorted_buffer_gt<candidate_t, std::less<candidate_t>, candidates_allocator_t>;
    using next_candidates_t = max_heap_gt<candidate_t, std::less<candidate_t>, candidates_allocator_t>;

    /**
     *  @brief  A loosely-structured handle for every node. One such node is created for every member.
     *          To minimize memory usage and maximize the number of entries per cache-line, it only
     *          stores to pointers. The internal tape starts with a `vector_key_t` @b key, then
     *          a `level_t` for the number of graph @b levels in which this member appears,
     *          then the { `neighbors_count_t`, `compressed_slot_t`, `compressed_slot_t` ... } sequences
     *          for @b each-level.
     */
    class node_t {
        byte_t* tape_{};

      public:
        explicit node_t(byte_t* tape) noexcept : tape_(tape) {}
        byte_t* tape() const noexcept { return tape_; }
        byte_t* neighbors_tape() const noexcept { return tape_ + node_head_bytes_(); }
        explicit operator bool() const noexcept { return tape_; }

        node_t() = default;
        node_t(node_t const&) = default;
        node_t& operator=(node_t const&) = default;

        misaligned_ref_gt<vector_key_t const> ckey() const noexcept { return {tape_}; }
        misaligned_ref_gt<vector_key_t const> ckey() noexcept { return {tape_}; }
        misaligned_ref_gt<vector_key_t const> key() const noexcept { return {tape_}; }
        misaligned_ref_gt<vector_key_t> key() noexcept { return {tape_}; }
        misaligned_ref_gt<level_t> level() noexcept { return {tape_ + sizeof(vector_key_t)}; }

        void key(vector_key_t v) noexcept { return misaligned_store<vector_key_t>(tape_, v); }
        void level(level_t v) noexcept { return misaligned_store<level_t>(tape_ + sizeof(vector_key_t), v); }
    };

    static_assert(std::is_trivially_copy_constructible<node_t>::value, "Nodes must be light!");
    static_assert(std::is_trivially_destructible<node_t>::value, "Nodes must be light!");

    /**
     *  @brief  A slice of the node's tape, containing a the list of neighbors
     *          for a node in a single graph level. It's pre-allocated to fit
     *          as many neighbors "slots", as may be needed at the target level,
     *          and starts with a single integer `neighbors_count_t` counter.
     */
    class neighbors_ref_t {
        byte_t* tape_;

        static constexpr std::size_t shift(std::size_t i = 0) noexcept {
            return sizeof(neighbors_count_t) + sizeof(compressed_slot_t) * i;
        }

      public:
        using iterator = misaligned_ptr_gt<compressed_slot_t>;
        using const_iterator = misaligned_ptr_gt<compressed_slot_t const>;
        using value_type = compressed_slot_t;

        neighbors_ref_t(byte_t* tape) noexcept : tape_(tape) {}
        misaligned_ptr_gt<compressed_slot_t> begin() noexcept { return tape_ + shift(); }
        misaligned_ptr_gt<compressed_slot_t> end() noexcept { return begin() + size(); }
        misaligned_ptr_gt<compressed_slot_t const> begin() const noexcept { return tape_ + shift(); }
        misaligned_ptr_gt<compressed_slot_t const> end() const noexcept { return begin() + size(); }
        misaligned_ptr_gt<compressed_slot_t const> cbegin() noexcept { return tape_ + shift(); }
        misaligned_ptr_gt<compressed_slot_t const> cend() noexcept { return cbegin() + size(); }
        compressed_slot_t operator[](std::size_t i) const noexcept {
            return misaligned_load<compressed_slot_t>(tape_ + shift(i));
        }
        std::size_t size() const noexcept { return misaligned_load<neighbors_count_t>(tape_); }
        void clear() noexcept {
            neighbors_count_t n = misaligned_load<neighbors_count_t>(tape_);
            std::memset(tape_, 0, shift(n));
            misaligned_store<neighbors_count_t>(tape_, 0);
        }
        void push_back(compressed_slot_t slot) noexcept {
            neighbors_count_t n = misaligned_load<neighbors_count_t>(tape_);
            misaligned_store<compressed_slot_t>(tape_ + shift(n), slot);
            misaligned_store<neighbors_count_t>(tape_, n + 1);
        }
        template <typename allow_slot_at> std::size_t erase_if(allow_slot_at&& allow_slot) noexcept {
            std::size_t old_count = misaligned_load<neighbors_count_t>(tape_);
            std::size_t removed_count = 0;
            for (std::size_t i = 0; i < old_count; ++i) {
                compressed_slot_t slot = misaligned_load<compressed_slot_t>(tape_ + shift(i));
                if (allow_slot(slot)) {
                    removed_count++;
                } else {
                    misaligned_store<compressed_slot_t>(tape_ + shift(i - removed_count), slot);
                }
            }
            misaligned_store<neighbors_count_t>(tape_, static_cast<neighbors_count_t>(old_count - removed_count));
            return removed_count;
        }
    };

    /**
     *  @brief  A package of all kinds of temporary data-structures, that the threads
     *          would reuse to process requests. Similar to having all of those as
     *          separate `thread_local` global variables.
     */
    struct usearch_align_m context_t {
        top_candidates_t top_candidates{};
        top_candidates_t top_for_refine{};
        next_candidates_t next_candidates{};
        visits_hash_set_t visits{};
        std::default_random_engine level_generator{};
        std::size_t iteration_cycles{};
        std::size_t computed_distances{};
        std::size_t computed_distances_in_refines{};
        std::size_t computed_distances_in_reverse_refines{};

        /// @brief Heterogeneous distance calculation.
        template <typename value_at, typename metric_at, typename entry_at> //
        inline distance_t measure(value_at const& first, entry_at const& second, metric_at&& metric) noexcept {
            static_assert( //
                std::is_same<entry_at, member_cref_t>::value || std::is_same<entry_at, member_citerator_t>::value,
                "Unexpected type");

            computed_distances++;
            return metric(first, second);
        }

        /// @brief Homogeneous distance calculation.
        template <typename metric_at, typename entry_at> //
        inline distance_t measure(entry_at const& first, entry_at const& second, metric_at&& metric) noexcept {
            static_assert( //
                std::is_same<entry_at, member_cref_t>::value || std::is_same<entry_at, member_citerator_t>::value,
                "Unexpected type");

            computed_distances++;
            return metric(first, second);
        }

        /// @brief Heterogeneous batch distance calculation.
        template <typename value_at, typename metric_at, typename entries_at, typename candidate_allowed_at,
                  typename transform_at,
                  typename callback_at> //
        inline void measure_batch(value_at const& first, entries_at const& second_entries, metric_at&& metric,
                                  candidate_allowed_at&& candidate_allowed, transform_at&& transform,
                                  callback_at&& callback) noexcept {

            using entry_t = typename std::remove_reference<decltype(second_entries[0])>::type;
            metric.batch(first, second_entries, candidate_allowed, transform,
                         [&](entry_t const& entry, distance_t distance) {
                             callback(entry, distance);
                             computed_distances++;
                         });
        }
    };

    /// @brief  Number of "slots" available for `node_t` objects. Equals to @b `limits_.members`.
    mutable std::atomic<std::size_t> nodes_capacity_{};

    /// @brief  Number of "slots" already storing non-null nodes.
    mutable std::atomic<std::size_t> nodes_count_{};

    index_config_t config_{};
    index_limits_t limits_{};

    mutable dynamic_allocator_t dynamic_allocator_{};
    tape_allocator_t tape_allocator_{};

    precomputed_constants_t pre_{};
    memory_mapped_file_t viewed_file_{};

    /// @brief  Controls access to `max_level_` and `entry_slot_`.
    ///         If any thread is updating those values, no other threads can `add()` or `search()`.
    std::mutex global_mutex_{};

    /// @brief  The level of the top-most graph in the index. Grows as the logarithm of size, starts from zero.
    level_t max_level_{};

    /// @brief  The slot in which the only node of the top-level graph is stored.
    std::size_t entry_slot_{};

    using nodes_allocator_t = typename dynamic_allocator_traits_t::template rebind_alloc<node_t>;

    /// @brief  C-style array of `node_t` smart-pointers. Use `compressed_slot_t` for indexing.
    buffer_gt<node_t, nodes_allocator_t> nodes_{};

    /// @brief  Mutex, that limits concurrent access to `nodes_`.
    mutable nodes_mutexes_t nodes_mutexes_{};

    using contexts_allocator_t = typename dynamic_allocator_traits_t::template rebind_alloc<context_t>;

    /// @brief  Array of thread-specific buffers for temporary data.
    mutable buffer_gt<context_t, contexts_allocator_t> contexts_{};

    context_t* context_or_null_(std::size_t thread) noexcept {
        return thread < contexts_.size() ? contexts_.data() + thread : nullptr;
    }

    context_t const* context_or_null_(std::size_t thread) const noexcept {
        return thread < contexts_.size() ? contexts_.data() + thread : nullptr;
    }

  public:
    std::size_t connectivity() const noexcept { return config_.connectivity; }
    std::size_t capacity() const noexcept { return nodes_capacity_; }
    std::size_t size() const noexcept { return nodes_count_; }
    std::size_t max_level() const noexcept { return nodes_count_ ? static_cast<std::size_t>(max_level_) : 0; }
    index_config_t const& config() const noexcept { return config_; }
    index_limits_t const& limits() const noexcept { return limits_; }
    bool is_immutable() const noexcept { return bool(viewed_file_); }
    explicit operator bool() const noexcept { return config_.is_valid(); }

    /**
     *  @brief Default index constructor, suitable only for stateless allocators.
     *  @warning Consider `index_gt::make` instead, or explicitly convert to `bool` to check if the index is valid.
     *  @section Exceptions
     *      Doesn't throw, unless the ::dynamic_allocator's and ::tape_allocator's throw on move-construction.
     */
    explicit index_gt( //
        dynamic_allocator_t dynamic_allocator = {}, tape_allocator_t tape_allocator = {}) noexcept(false)
        : nodes_capacity_(0u), nodes_count_(0u), config_(), limits_(0, 0),
          dynamic_allocator_(std::move(dynamic_allocator)), tape_allocator_(std::move(tape_allocator)),
          pre_(precompute_({})), max_level_(-1), entry_slot_(0u), nodes_(), nodes_mutexes_(), contexts_() {}

    /**
     *  @brief Default index constructor, suitable only for stateless allocators.
     *  @warning Consider `index_gt::make` instead, or explicitly convert to `bool` to check if the index is valid.
     *  @section Exceptions
     *      Doesn't throw, unless the ::dynamic_allocator's and ::tape_allocator's throw on move-construction.
     */
    explicit index_gt(index_config_t config, dynamic_allocator_t dynamic_allocator = {},
                      tape_allocator_t tape_allocator = {}) noexcept(false)
        : index_gt(dynamic_allocator, tape_allocator) {
        config.validate();
        config_ = config;
        pre_ = precompute_(config);
    }

    /**
     *  @brief  Clones the structure with the same hyper-parameters, but without contents.
     */
    index_gt fork() noexcept { return index_gt{config_, dynamic_allocator_, tape_allocator_}; }

    ~index_gt() noexcept { reset(); }

    index_gt(index_gt&& other) noexcept { swap(other); }

    index_gt& operator=(index_gt&& other) noexcept {
        swap(other);
        return *this;
    }

    struct state_result_t {
        index_gt index;
        error_t error;

        explicit operator bool() const noexcept { return !error; }
        state_result_t failed(error_t message) noexcept { return {std::move(index), std::move(message)}; }
        operator index_gt&&() && {
            if (error)
                usearch_raise_runtime_error(error.what());
            return std::move(index);
        }
    };
    using copy_result_t = state_result_t;

    /**
     *  @brief  The recommended way to initialize the index, as unlike the constructor,
     *          it can fail with an error message, without raising an exception.
     *
     *  @param[in] config The configuration specs of the index.
     *  @param[in] dynamic_allocator The allocator for temporary buffers and thread contexts, like priority queues.
     *  @param[in] tape_allocator The allocator for the primary allocations of nodes and vectors.
     */
    static state_result_t make( //
        index_config_t config = {}, dynamic_allocator_t dynamic_allocator = {},
        tape_allocator_t tape_allocator = {}) noexcept {

        state_result_t result;
        result.error = config.validate();
        if (result.error)
            return result;

        index_gt index;
        index.config_ = std::move(config);
        index.dynamic_allocator_ = std::move(dynamic_allocator);
        index.tape_allocator_ = std::move(tape_allocator);
        index.pre_ = precompute_(index.config_);
        index.nodes_count_ = 0u;
        index.max_level_ = -1;
        index.entry_slot_ = 0u;

        result.index = std::move(index);
        return result;
    }

    /**
     *  @brief  The recommended way to copy the index, as unlike the copy-constructor,
     *          it can fail with an error message, without raising an exception.
     *
     *  @param[in] config The configuration specs for the copy-operation. Currently unused.
     */
    copy_result_t copy(index_copy_config_t config = {}) const noexcept {
        copy_result_t result;
        index_gt& other = result.index;
        other = index_gt(config_, dynamic_allocator_, tape_allocator_);
        if (!other.reserve(limits_))
            return result.failed("Failed to reserve the contexts");

        // Now all is left - is to allocate new `node_t` instances and populate
        // the `other.nodes_` array into it.
        for (std::size_t i = 0; i != nodes_count_; ++i)
            other.nodes_[i] = other.node_make_copy_(node_bytes_(nodes_[i]));

        other.nodes_count_ = nodes_count_.load();
        other.max_level_ = max_level_;
        other.entry_slot_ = entry_slot_;

        // This controls nothing for now :)
        (void)config;
        return result;
    }

    member_citerator_t cbegin() const noexcept { return {this, static_cast<compressed_slot_t>(0u)}; }
    member_citerator_t cend() const noexcept { return {this, static_cast<compressed_slot_t>(size())}; }
    member_citerator_t begin() const noexcept { return {this, static_cast<compressed_slot_t>(0u)}; }
    member_citerator_t end() const noexcept { return {this, static_cast<compressed_slot_t>(size())}; }
    member_iterator_t begin() noexcept { return {this, static_cast<compressed_slot_t>(0u)}; }
    member_iterator_t end() noexcept { return {this, static_cast<compressed_slot_t>(size())}; }

    member_ref_t at(compressed_slot_t slot) noexcept { return {nodes_[slot].key(), slot}; }
    member_cref_t at(compressed_slot_t slot) const noexcept { return {nodes_[slot].ckey(), slot}; }
    member_iterator_t iterator_at(compressed_slot_t slot) noexcept { return {this, slot}; }
    member_citerator_t citerator_at(compressed_slot_t slot) const noexcept { return {this, slot}; }

    /**
     *  @brief  A read-only random-access range over the neighbors of a single
     *          node at a single graph level. Dereferencing yields a `member_cref_t`,
     *          so callers can chain traversals without touching internal slots.
     *
     *  @warning The range aliases the node's adjacency tape. It is only valid
     *           while the index is not being mutated. Prefer immutable indexes
     *           (see `is_immutable()`) or guarantee no concurrent `add`/`update`/
     *           `remove` while the view is alive.
     */
    class neighbors_view_t {
        index_gt const* index_{};
        neighbors_ref_t neighbors_{nullptr};

      public:
        class const_iterator {
            index_gt const* index_{};
            misaligned_ptr_gt<compressed_slot_t const> position_{nullptr};

          public:
            using iterator_category = std::random_access_iterator_tag;
            using value_type = member_cref_t;
            using difference_type = std::ptrdiff_t;
            using pointer = void;
            using reference = member_cref_t;

            const_iterator() noexcept = default;
            const_iterator(index_gt const* index, misaligned_ptr_gt<compressed_slot_t const> position) noexcept
                : index_(index), position_(position) {}

            reference operator*() const noexcept {
                compressed_slot_t slot = static_cast<compressed_slot_t>(*position_);
                return {index_->node_at_(slot).ckey(), slot};
            }
            compressed_slot_t slot() const noexcept { return static_cast<compressed_slot_t>(*position_); }

            // clang-format off
            const_iterator& operator++() noexcept { ++position_; return *this; }
            const_iterator operator++(int) noexcept { const_iterator old = *this; ++position_; return old; }
            const_iterator& operator--() noexcept { --position_; return *this; }
            const_iterator operator--(int) noexcept { const_iterator old = *this; --position_; return old; }
            const_iterator& operator+=(difference_type d) noexcept { position_ = position_ + d; return *this; }
            const_iterator& operator-=(difference_type d) noexcept { position_ = position_ - d; return *this; }
            const_iterator operator+(difference_type d) const noexcept { return {index_, position_ + d}; }
            const_iterator operator-(difference_type d) const noexcept { return {index_, position_ - d}; }
            difference_type operator-(const_iterator const& other) const noexcept { return position_ - other.position_; }
            bool operator==(const_iterator const& other) const noexcept { return position_ == other.position_; }
            bool operator!=(const_iterator const& other) const noexcept { return position_ != other.position_; }
            // clang-format on
        };

        using iterator = const_iterator;
        using value_type = member_cref_t;
        using size_type = std::size_t;

        neighbors_view_t() noexcept = default;
        neighbors_view_t(index_gt const* index, neighbors_ref_t neighbors) noexcept
            : index_(index), neighbors_(neighbors) {}

        std::size_t size() const noexcept { return index_ ? neighbors_.size() : 0; }
        bool empty() const noexcept { return size() == 0; }
        member_cref_t operator[](std::size_t offset) const noexcept {
            compressed_slot_t slot = neighbors_[offset];
            return {index_->node_at_(slot).ckey(), slot};
        }
        const_iterator begin() const noexcept {
            return index_ ? const_iterator{index_, neighbors_.begin()} : const_iterator{};
        }
        const_iterator end() const noexcept {
            return index_ ? const_iterator{index_, neighbors_.end()} : const_iterator{};
        }
        const_iterator cbegin() const noexcept { return begin(); }
        const_iterator cend() const noexcept { return end(); }
    };

    /**
     *  @brief  Returns a read-only range over the neighbors of the node at @p slot
     *          in the graph @p level. Returned view is empty when @p level exceeds
     *          the node's level.
     */
    neighbors_view_t neighbors(compressed_slot_t slot, std::size_t level) const noexcept {
        node_t node = node_at_(slot);
        if (static_cast<level_t>(level) > node.level())
            return {};
        return {this, neighbors_(node, static_cast<level_t>(level))};
    }

    /**
     *  @brief  Returns a read-only range over the neighbors of the node referenced
     *          by @p member at the graph @p level.
     */
    neighbors_view_t neighbors(member_citerator_t member, std::size_t level) const noexcept {
        return neighbors(get_slot(member), level);
    }

    /**
     *  @brief  Returns the top graph level at which the node at @p slot is present.
     */
    std::size_t level_of(compressed_slot_t slot) const noexcept {
        return static_cast<std::size_t>(static_cast<level_t>(node_at_(slot).level()));
    }

    /**
     *  @brief  Returns the top graph level at which the node referenced by @p member
     *          is present.
     */
    std::size_t level_of(member_citerator_t member) const noexcept { return level_of(get_slot(member)); }

    dynamic_allocator_t const& dynamic_allocator() const noexcept { return dynamic_allocator_; }
    tape_allocator_t const& tape_allocator() const noexcept { return tape_allocator_; }

#if defined(USEARCH_USE_PRAGMA_REGION)
#pragma region Adjusting Configuration
#endif

    /**
     *  @brief Erases all the vectors from the index.
     *
     *  Will change `size()` to zero, but will keep the same `capacity()`.
     *  Will keep the number of available threads/contexts the same as it was.
     */
    void clear() noexcept {
        if (!has_reset<tape_allocator_t>()) {
            std::size_t n = nodes_count_;
            for (std::size_t i = 0; i != n; ++i)
                node_free_(i);
        } else
            tape_allocator_.deallocate(nullptr, 0);
        nodes_count_ = 0;
        max_level_ = -1;
        entry_slot_ = 0u;
    }

    /**
     *  @brief Erases all members from index, closing files, and returning RAM to OS.
     *
     *  Will change both `size()` and `capacity()` to zero.
     *  Will deallocate all threads/contexts.
     *  If the index is memory-mapped - releases the mapping and the descriptor.
     */
    void reset() noexcept {
        clear();

        nodes_ = {};
        contexts_ = {};
        nodes_mutexes_ = {};
        limits_ = index_limits_t{0, 0};
        nodes_capacity_ = 0;
        viewed_file_ = memory_mapped_file_t{};
        tape_allocator_ = {};
    }

    /**
     *  @brief  Swaps the underlying memory buffers and thread contexts.
     */
    void swap(index_gt& other) noexcept {
        std::swap(config_, other.config_);
        std::swap(limits_, other.limits_);
        std::swap(dynamic_allocator_, other.dynamic_allocator_);
        std::swap(tape_allocator_, other.tape_allocator_);
        std::swap(pre_, other.pre_);
        std::swap(viewed_file_, other.viewed_file_);
        std::swap(max_level_, other.max_level_);
        std::swap(entry_slot_, other.entry_slot_);
        std::swap(nodes_, other.nodes_);
        std::swap(nodes_mutexes_, other.nodes_mutexes_);
        std::swap(contexts_, other.contexts_);

        // Non-atomic parts.
        std::size_t capacity_copy = nodes_capacity_;
        std::size_t count_copy = nodes_count_;
        nodes_capacity_ = other.nodes_capacity_.load();
        nodes_count_ = other.nodes_count_.load();
        other.nodes_capacity_ = capacity_copy;
        other.nodes_count_ = count_copy;
    }

    /**
     *  @brief  Increases the `capacity()` of the index to allow adding more vectors.
     *  @return `true` on success, `false` on memory allocation errors.
     */
    bool try_reserve(index_limits_t limits) usearch_noexcept_m {

        if (limits.threads_add <= limits_.threads_add          //
            && limits.threads_search <= limits_.threads_search //
            && limits.members <= limits_.members)
            return true;

        // In some cases, we don't want to update the number of members,
        // just want to make sure that future reserves use the new thread limits.
        if (!limits.members && !size()) {
            limits_ = limits;
            return true;
        }

        std::size_t connectivity_max = (std::max)(config_.connectivity_base, config_.connectivity);
        nodes_mutexes_t new_mutexes(limits.threads(), connectivity_max);
        buffer_gt<node_t, nodes_allocator_t> new_nodes(limits.members);
        buffer_gt<context_t, contexts_allocator_t> new_contexts(limits.threads());
        if (!new_nodes || !new_contexts || !new_mutexes)
            return false;

        // Move the nodes info, and deallocate previous buffers.
        if (nodes_)
            std::memcpy(new_nodes.data(), nodes_.data(), sizeof(node_t) * size());
        for (std::size_t i = 0; i != new_contexts.size(); ++i)
            if (!new_contexts[i].top_for_refine.reserve(connectivity_max + 1))
                return false;

        limits_ = limits;
        nodes_capacity_ = limits.members;
        nodes_ = std::move(new_nodes);
        contexts_ = std::move(new_contexts);
        nodes_mutexes_ = std::move(new_mutexes);
        return true;
    }

    /**
     *  @brief Increases the `capacity()` of the index to allow adding more vectors.
     *  @warning Unlike STL, won't throw exceptions on memory allocations, so check the return value.
     *  @return `true` on success, `false` on memory allocation errors.
     */
    bool reserve(index_limits_t limits) usearch_noexcept_m { return try_reserve(limits); }

#if defined(USEARCH_USE_PRAGMA_REGION)
#pragma endregion

#pragma region Construction and Search
#endif

    struct add_result_t {
        error_t error{};
        std::size_t new_size{};
        std::size_t visited_members{};
        std::size_t computed_distances{};
        std::size_t computed_distances_in_refines{};
        std::size_t computed_distances_in_reverse_refines{};
        compressed_slot_t slot{};

        explicit operator bool() const noexcept { return !error; }
        add_result_t failed(error_t message) noexcept {
            error = std::move(message);
            return std::move(*this);
        }
    };

    /// @brief  Describes a matched search result, augmenting `member_cref_t`
    ///         contents with `distance` to the query object.
    struct match_t {
        member_cref_t member;
        distance_t distance;

        inline match_t() noexcept : member({nullptr, 0}), distance((std::numeric_limits<distance_t>::max)()) {}

        inline match_t(member_cref_t member, distance_t distance) noexcept : member(member), distance(distance) {}

        inline match_t(match_t&& other) noexcept
            : member({other.member.key.ptr(), other.member.slot}), distance(other.distance) {}

        inline match_t(match_t const& other) noexcept
            : member({other.member.key.ptr(), other.member.slot}), distance(other.distance) {}

        inline match_t& operator=(match_t const& other) noexcept {
            member.key.reset(other.member.key.ptr());
            member.slot = other.member.slot;
            distance = other.distance;
            return *this;
        }

        inline match_t& operator=(match_t&& other) noexcept {
            member.key.reset(other.member.key.ptr());
            member.slot = other.member.slot;
            distance = other.distance;
            return *this;
        }
    };

    class search_result_t {
        node_t const* nodes_{};
        top_candidates_t const* top_{};

        friend class index_gt;
        inline search_result_t(index_gt const& index, top_candidates_t const* top) noexcept
            : nodes_(index.nodes_), top_(top) {}

      public:
        /**  @brief  Number of search results found. */
        std::size_t count{};
        /**  @brief  Number of graph nodes traversed. */
        std::size_t visited_members{};
        /**  @brief  Number of times the distances were computed. */
        std::size_t computed_distances{};
        error_t error{};

        inline search_result_t() noexcept {}
        inline search_result_t(search_result_t&&) = default;
        inline search_result_t& operator=(search_result_t&&) = default;

        explicit operator bool() const noexcept { return !error; }
        search_result_t failed(error_t message) noexcept {
            error = std::move(message);
            return std::move(*this);
        }

        inline operator std::size_t() const noexcept { return count; }
        inline std::size_t size() const noexcept { return count; }
        inline bool empty() const noexcept { return !count; }
        inline match_t operator[](std::size_t i) const noexcept { return at(i); }
        inline match_t front() const noexcept { return at(0); }
        inline match_t back() const noexcept {
            usearch_assert_m(count > 0, "Can't call back() on an empty result set");
            return at(count - 1);
        }
        inline bool contains(vector_key_t key) const noexcept {
            for (std::size_t i = 0; i != count; ++i)
                if (at(i).member.key == key)
                    return true;
            return false;
        }
        inline match_t at(std::size_t i) const noexcept {
            candidate_t const* top_ordered = top_->data();
            candidate_t candidate = top_ordered[i];
            node_t node = nodes_[candidate.slot];
            return {member_cref_t{node.ckey(), candidate.slot}, candidate.distance};
        }

        /**
         *  @brief  Extracts the search results into a user-provided buffer, that unlike `dump_to`,
         *          may already contain some data, so the new and old results are merged together.
         *  @return The number of results stored in the buffer.
         *  @param[in] keys The buffer to store the keys of the search results.
         *  @param[in] distances The buffer to store the distances to the search results.
         *  @param[in] old_count The number of results already stored in the buffers.
         *  @param[in] max_count The maximum number of results that can be stored in the buffers.
         */
        inline std::size_t merge_into(                 //
            vector_key_t* keys, distance_t* distances, //
            std::size_t old_count, std::size_t max_count) const noexcept {

            std::size_t merged_count = old_count;
            for (std::size_t i = 0; i != count; ++i) {
                match_t result = operator[](i);
                distance_t* merged_end = distances + merged_count;
                std::size_t offset = std::lower_bound(distances, merged_end, result.distance) - distances;
                if (offset == max_count)
                    continue;

                std::size_t count_worse = merged_count - offset - (max_count == merged_count);
                std::memmove(keys + offset + 1, keys + offset, count_worse * sizeof(vector_key_t));
                std::memmove(distances + offset + 1, distances + offset, count_worse * sizeof(distance_t));
                keys[offset] = result.member.key;
                distances[offset] = result.distance;
                merged_count += merged_count != max_count;
            }
            return merged_count;
        }

        /**
         *  @brief  Extracts the search results into a user-provided buffer.
         *  @return The number of results stored in the buffer.
         *  @param[in] keys The buffer to store the keys of the search results.
         *  @param[in] distances The buffer to store the distances to the search results.
         */
        inline std::size_t dump_to(vector_key_t* keys, distance_t* distances) const noexcept {
            for (std::size_t i = 0; i != count; ++i) {
                match_t result = operator[](i);
                keys[i] = result.member.key;
                distances[i] = result.distance;
            }
            return count;
        }

        /**
         *  @brief  Extracts the search results into a user-provided buffer.
         *  @return The number of results stored in the buffer.
         *  @param[in] keys The buffer to store the keys of the search results.
         */
        inline std::size_t dump_to(vector_key_t* keys) const noexcept {
            for (std::size_t i = 0; i != count; ++i) {
                match_t result = operator[](i);
                keys[i] = result.member.key;
            }
            return count;
        }

        /**
         *  @brief  Extracts the search results into a user-provided buffer.
         *  @return The number of results stored in the buffer.
         *  @param[in] keys The buffer to store the keys of the search results.
         *  @param[in] distances The buffer to store the distances to the search results.
         *  @param[in] capacity The maximum number of results that can be stored in the buffers.
         */
        inline std::size_t dump_to(vector_key_t* keys, distance_t* distances, std::size_t capacity) const noexcept {
            std::size_t i = 0;
            std::size_t initialized_count = (std::min)(count, capacity);
            for (; i != initialized_count; ++i) {
                match_t result = operator[](i);
                keys[i] = result.member.key;
                distances[i] = result.distance;
            }
            for (; i != capacity; ++i) {
                keys[i] = vector_key_t{};
                distances[i] = std::numeric_limits<distance_t>::has_signaling_NaN
                                   ? std::numeric_limits<distance_t>::signaling_NaN()
                                   : (std::numeric_limits<distance_t>::max)();
            }
            return initialized_count;
        }

        /**
         *  @brief  Extracts the search results into a user-provided buffer.
         *  @return The number of results stored in the buffer.
         *  @param[in] keys The buffer to store the keys of the search results.
         *  @param[in] capacity The maximum number of results that can be stored in the buffers.
         */
        inline std::size_t dump_to(vector_key_t* keys, std::size_t capacity) const noexcept {
            std::size_t i = 0;
            std::size_t initialized_count = (std::min)(this->count, capacity);
            for (; i != initialized_count; ++i) {
                match_t result = operator[](i);
                keys[i] = result.member.key;
            }
            for (; i != capacity; ++i)
                keys[i] = vector_key_t{};

            return initialized_count;
        }
    };

    struct cluster_result_t {
        error_t error{};
        std::size_t visited_members{};
        std::size_t computed_distances{};
        match_t cluster{};

        explicit operator bool() const noexcept { return !error; }
        cluster_result_t failed(error_t message) noexcept {
            error = std::move(message);
            return std::move(*this);
        }
    };

    /**
     *  @brief  Inserts a new entry into the index. Thread-safe. Supports @b heterogeneous lookups.
     *          Expects needed capacity to be reserved ahead of time: `size() < capacity()`.
     *
     *  @tparam metric_at
     *      A function responsible for computing the distance @b (dis-similarity) between two objects.
     *      It should be callable into distinctly different scenarios:
     *          - `distance_t operator() (value_at, entry_at)` - from new object to existing entries.
     *          - `distance_t operator() (entry_at, entry_at)` - between existing entries.
     *      Where any possible `entry_at` has both two interfaces: `std::size_t slot()`, `vector_key_t key()`.
     *
     *  @param[in] key External identifier/name/descriptor for the new entry.
     *  @param[in] value Content that will be compared against other entries to index.
     *  @param[in] metric Callable object measuring distance between ::value and present objects.
     *  @param[in] config Configuration options for this specific operation.
     *  @param[in] callback On-success callback, executed while the `member_ref_t` is still under lock.
     */
    template <                                   //
        typename value_at,                       //
        typename metric_at,                      //
        typename callback_at = dummy_callback_t, //
        typename prefetch_at = dummy_prefetch_t  //
        >
    add_result_t add(                                           //
        vector_key_t key, value_at&& value, metric_at&& metric, //
        index_update_config_t config = {},                      //
        callback_at&& callback = callback_at{},                 //
        prefetch_at&& prefetch = prefetch_at{}) usearch_noexcept_m {

        // Zero expansion is meaningless, fall back to default
        if (!config.expansion)
            config.expansion = default_expansion_add();

        add_result_t result;
        if (is_immutable())
            return result.failed("Can't add to an immutable index");

        // Make sure we have enough local memory to perform this request
        context_t* context_ptr = context_or_null_(config.thread);
        if (!context_ptr)
            return result.failed("Reserve capacity ahead of insertions!");
        context_t& context = *context_ptr;
        top_candidates_t& top = context.top_candidates;
        next_candidates_t& next = context.next_candidates;
        top.clear();
        next.clear();

        // The top list needs one more slot than the connectivity of the base level
        // for the heuristic, that tries to squeeze one more element into saturated list.
        std::size_t connectivity_max = (std::max)(config_.connectivity_base, config_.connectivity);
        std::size_t top_limit = (std::max)(connectivity_max + 1, config.expansion);
        if (!top.reserve(top_limit))
            return result.failed("Out of memory!");
        if (!next.reserve(config.expansion))
            return result.failed("Out of memory!");

        // Determining how much memory to allocate for the node depends on the target level
        std::unique_lock<std::mutex> new_level_lock(global_mutex_);
        level_t max_level_copy = max_level_;                                             // Copy under lock
        compressed_slot_t entry_slot_copy = static_cast<compressed_slot_t>(entry_slot_); // Copy under lock
        level_t new_target_level = choose_random_level_(context.level_generator);

        // Make sure we are not overflowing
        std::size_t capacity = nodes_capacity_.load();
        std::size_t old_size = nodes_count_.fetch_add(1);
        if (old_size >= capacity) {
            nodes_count_.fetch_sub(1);
            return result.failed("Reserve capacity ahead of insertions!");
        }

        // Allocate the neighbors
        node_t new_node = node_make_(key, new_target_level);
        if (!new_node) {
            nodes_count_.fetch_sub(1);
            return result.failed("Out of memory!");
        }
        if (new_target_level <= max_level_copy)
            new_level_lock.unlock();

        nodes_[old_size] = new_node;
        result.new_size = old_size + 1;
        compressed_slot_t new_slot = result.slot = static_cast<compressed_slot_t>(old_size);
        callback(at(result.slot));

        // Do nothing for the first element
        if (!old_size) {
            entry_slot_ = result.slot;
            max_level_ = new_target_level;
            return result;
        }

        // Pull stats
        result.computed_distances = context.computed_distances;
        result.computed_distances_in_refines = context.computed_distances_in_refines;
        result.computed_distances_in_reverse_refines = context.computed_distances_in_reverse_refines;
        result.visited_members = context.iteration_cycles;

        // Go down the level, tracking only the closest match
        compressed_slot_t closest_slot = search_for_one_( //
            value, metric, prefetch,                      //
            entry_slot_copy, max_level_copy, new_target_level, context);

        // From `new_target_level` down - perform proper extensive search
        // linked_hi：已成功连边的最高层（含）；-1 表示尚未连任何层。OOM 时只清这些层的反向边。
        level_t linked_hi = -1;
        for (level_t level = (std::min)(new_target_level, max_level_copy); level >= 0; --level) {
            // search_to_insert_ 已在 visits/top/next 增长失败时返回 false；忽略会导致半成品图继续连边。
            if (!search_to_insert_(value, metric, prefetch, closest_slot, level, config.expansion, context)) {
                if (linked_hi >= 0)
                    unlink_slot_(new_slot, linked_hi);
                node_free_(new_slot);
                nodes_count_.fetch_sub(1);
                return result.failed("Out of memory during graph expansion");
            }
            candidates_view_t closest_view;
            {
                node_lock_t new_lock = node_lock_(new_slot);
                neighbors_(new_node, level).clear();
                closest_view = form_links_to_closest_(metric, new_slot, level, context);
                closest_slot = closest_view[0].slot;
            }
            form_reverse_links_(metric, new_slot, closest_view, value, level, context);
            if (linked_hi < 0)
                linked_hi = level;
        }

        // Normalize stats
        result.computed_distances = context.computed_distances - result.computed_distances;
        result.computed_distances_in_refines =
            context.computed_distances_in_refines - result.computed_distances_in_refines;
        result.computed_distances_in_reverse_refines =
            context.computed_distances_in_reverse_refines - result.computed_distances_in_reverse_refines;
        result.visited_members = context.iteration_cycles - result.visited_members;

        // Updating the entry point if needed
        if (new_target_level > max_level_copy) {
            entry_slot_ = new_slot;
            max_level_ = new_target_level;
        }
        return result;
    }

    /**
     *  @brief  Update an existing entry. Thread-safe. Supports @b heterogeneous lookups.
     *
     *  ! It's assumed that different threads aren't updating the same entry at the same time.
     *  ! The state won't be corrupted, but no transactional guarantees are provided and the
     *  ! resulting value & neighbors list may be inconsistent.
     *
     *  @tparam metric_at
     *      A function responsible for computing the distance @b (dis-similarity) between two objects.
     *      It should be callable into distinctly different scenarios:
     *          - `distance_t operator() (value_at, entry_at)` - from new object to existing entries.
     *          - `distance_t operator() (entry_at, entry_at)` - between existing entries.
     *      For any possible `entry_at` following interfaces will work:
     *          - `std::size_t get_slot(entry_at const &)`
     *          - `vector_key_t get_key(entry_at const &)`
     *
     *  @param[in] iterator Iterator pointing to an existing entry to be replaced.
     *  @param[in] key External identifier/name/descriptor for the entry.
     *  @param[in] value Content that will be compared against other entries in the index.
     *  @param[in] metric Callable object measuring distance between ::value and present objects.
     *  @param[in] config Configuration options for this specific operation.
     *  @param[in] callback On-success callback, executed while the `member_ref_t` is still under lock.
     */
    template <                                   //
        typename value_at,                       //
        typename metric_at,                      //
        typename callback_at = dummy_callback_t, //
        typename prefetch_at = dummy_prefetch_t  //
        >
    add_result_t update(                        //
        member_iterator_t iterator,             //
        vector_key_t key,                       //
        value_at&& value,                       //
        metric_at&& metric,                     //
        index_update_config_t config = {},      //
        callback_at&& callback = callback_at{}, //
        prefetch_at&& prefetch = prefetch_at{}) usearch_noexcept_m {

        // Someone is gonna fuzz this, so let's make sure we cover the basics
        if (!config.expansion)
            config.expansion = default_expansion_add();

        usearch_assert_m(!is_immutable(), "Can't add to an immutable index");
        add_result_t result;
        compressed_slot_t updated_slot = iterator.slot_;

        // Make sure we have enough local memory to perform this request
        context_t* context_ptr = context_or_null_(config.thread);
        if (!context_ptr)
            return result.failed("Reserve capacity ahead of updates!");
        context_t& context = *context_ptr;
        top_candidates_t& top = context.top_candidates;
        next_candidates_t& next = context.next_candidates;
        top.clear();
        next.clear();

        // The top list needs one more slot than the connectivity of the base level
        // for the heuristic, that tries to squeeze one more element into saturated list.
        std::size_t connectivity_max = (std::max)(config_.connectivity_base, config_.connectivity);
        std::size_t top_limit = (std::max)(connectivity_max + 1, config.expansion);
        if (!top.reserve(top_limit))
            return result.failed("Out of memory!");
        if (!next.reserve(config.expansion))
            return result.failed("Out of memory!");

        node_t updated_node = node_at_(updated_slot);
        level_t updated_node_level = updated_node.level();

        // Copy entry coordinates under locks
        level_t max_level_copy;
        compressed_slot_t entry_slot_copy;
        {
            std::unique_lock<std::mutex> new_level_lock(global_mutex_);
            max_level_copy = max_level_;                                   // Copy under lock
            entry_slot_copy = static_cast<compressed_slot_t>(entry_slot_); // Copy under lock
        }

        // Pull stats
        result.computed_distances = context.computed_distances;
        result.visited_members = context.iteration_cycles;

        // Go down the level, tracking only the closest match;
        // It may even be equal to the `updated_slot`
        compressed_slot_t closest_slot =
            // If we are updating the entry node itself, it won't contain any neighbors,
            // so we should traverse a level down to find the closest match.
            updated_node_level == max_level_copy //
                ? entry_slot_copy
                : search_for_one_(             //
                      value, metric, prefetch, //
                      entry_slot_copy, max_level_copy, updated_node_level, context);

        // From `updated_node_level` down - perform proper extensive search
        for (level_t level = (std::min)(updated_node_level, max_level_copy); level >= 0; --level) {
            if (!search_to_update_(value, metric, prefetch, closest_slot, updated_slot, level, config.expansion,
                                   context))
                return result.failed("Out of memory!");

            candidates_view_t closest_view;
            {
                node_lock_t updated_lock = node_lock_(updated_slot);
                // TODO: Go through existing neighbors removing reverse links
                // for (compressed_slot_t slot : neighbors_(updated_node, level))
                //     remove_link_(slot, updated_slot, level);
                neighbors_(updated_node, level).clear();
                closest_view = form_links_to_closest_(metric, updated_slot, level, context);
                if (closest_view.size())
                    closest_slot = closest_view[0].slot;
            }
            form_reverse_links_(metric, updated_slot, closest_view, value, level, context);
        }
        if (static_cast<vector_key_t>(updated_node.key()) != key)
            updated_node.key(key);

        // Normalize stats
        result.computed_distances = context.computed_distances - result.computed_distances;
        result.visited_members = context.iteration_cycles - result.visited_members;
        result.slot = updated_slot;

        callback(at(updated_slot));
        return result;
    }

    /**
     *  @brief Searches for the closest elements to the given ::query. Thread-safe.
     *
     *  @param[in] query Content that will be compared against other entries in the index.
     *  @param[in] wanted The upper bound for the number of results to return.
     *  @param[in] config Configuration options for this specific operation.
     *  @param[in] predicate Optional filtering predicate for `member_cref_t`.
     *  @return Smart object referencing temporary memory. Valid until next `search()`, `add()`, or `cluster()`.
     */
    template <                                     //
        typename value_at,                         //
        typename metric_at,                        //
        typename predicate_at = dummy_predicate_t, //
        typename prefetch_at = dummy_prefetch_t    //
        >
    search_result_t search(                        //
        value_at&& query,                          //
        std::size_t wanted,                        //
        metric_at&& metric,                        //
        index_search_config_t config = {},         //
        predicate_at&& predicate = predicate_at{}, //
        prefetch_at&& prefetch = prefetch_at{}) const usearch_noexcept_m {

        // Someone is gonna fuzz this, so let's make sure we cover the basics
        if (!wanted)
            return search_result_t{};

        // Expansion factor set to zero is equivalent to the default value
        if (!config.expansion)
            config.expansion = default_expansion_search();

        // Using references is cleaner, but would result in UBSan false positives
        context_t* context_ptr = contexts_.data() ? contexts_.data() + config.thread : nullptr;
        top_candidates_t* top_ptr = context_ptr ? &context_ptr->top_candidates : nullptr;
        search_result_t result{*this, top_ptr};
        if (!nodes_count_.load(std::memory_order_relaxed))
            return result;

        usearch_assert_m(contexts_.size() > config.thread, "Thread index out of bounds");
        context_t& context = *context_ptr;
        top_candidates_t& top = *top_ptr;
        // Go down the level, tracking only the closest match
        result.computed_distances = context.computed_distances;
        result.visited_members = context.iteration_cycles;

        if (config.exact) {
            if (!top.reserve(wanted))
                return result.failed("Out of memory!");
            search_exact_(query, metric, predicate, wanted, context);
        } else {
            next_candidates_t& next = context.next_candidates;
            std::size_t expansion = (std::max)(config.expansion, wanted);
            usearch_assert_m(expansion > 0, "Expansion factor can't be a zero!");
            if (!next.reserve(expansion))
                return result.failed("Out of memory!");
            if (!top.reserve(expansion))
                return result.failed("Out of memory!");

            compressed_slot_t closest_slot = search_for_one_(
                query, metric, prefetch, static_cast<compressed_slot_t>(entry_slot_), max_level_, 0, context);

            // For bottom layer we need a more optimized procedure
            if (!search_to_find_in_base_(query, metric, predicate, prefetch, closest_slot, expansion, context))
                return result.failed("Out of memory!");
        }

        top.sort_ascending();
        top.shrink(wanted);

        // Normalize stats
        result.computed_distances = context.computed_distances - result.computed_distances;
        result.visited_members = context.iteration_cycles - result.visited_members;
        result.count = top.size();
        return result;
    }

    /**
     *  @brief Identifies the closest cluster to the given ::query. Thread-safe.
     *
     *  @param[in] query Content that will be compared against other entries in the index.
     *  @param[in] level The index level to target. Higher means lower resolution.
     *  @param[in] config Configuration options for this specific operation.
     *  @param[in] predicate Optional filtering predicate for `member_cref_t`.
     *  @return Smart object referencing temporary memory. Valid until next `search()`, `add()`, or `cluster()`.
     */
    template <                                     //
        typename value_at,                         //
        typename metric_at,                        //
        typename predicate_at = dummy_predicate_t, //
        typename prefetch_at = dummy_prefetch_t    //
        >
    cluster_result_t cluster(                      //
        value_at&& query,                          //
        std::size_t level,                         //
        metric_at&& metric,                        //
        index_cluster_config_t config = {},        //
        predicate_at&& predicate = predicate_at{}, //
        prefetch_at&& prefetch = prefetch_at{}) const noexcept {

        if (!config.expansion)
            config.expansion = default_expansion_search();

        context_t& context = contexts_[config.thread];
        cluster_result_t result;
        if (!nodes_count_)
            return result.failed("No clusters to identify");

        // Go down the level, tracking only the closest match
        result.computed_distances = context.computed_distances;
        result.visited_members = context.iteration_cycles;

        next_candidates_t& next = context.next_candidates;
        std::size_t expansion = config.expansion;
        if (!next.reserve(expansion))
            return result.failed("Out of memory!");

        result.cluster.member =
            at(search_for_one_(query, metric, prefetch, static_cast<compressed_slot_t>(entry_slot_), max_level_,
                               static_cast<level_t>(level <= 0 ? 0 : level - 1), context));
        result.cluster.distance = context.measure(query, result.cluster.member, metric);

        // Normalize stats
        result.computed_distances = context.computed_distances - result.computed_distances;
        result.visited_members = context.iteration_cycles - result.visited_members;

        (void)predicate;
        return result;
    }

#if defined(USEARCH_USE_PRAGMA_REGION)
#pragma endregion

#pragma region Metadata
#endif

    struct stats_t {
        std::size_t nodes{};
        std::size_t edges{};
        std::size_t max_edges{};
        std::size_t allocated_bytes{};
    };

    /**
     *  @brief  Aggregates stats on the number of nodes, edges, and memory usage across all levels.
     */
    stats_t stats() const noexcept {
        stats_t result{};

        for (std::size_t i = 0; i != size(); ++i) {
            node_t node = node_at_(i);
            std::size_t max_edges = node.level() * config_.connectivity + config_.connectivity_base;
            std::size_t edges = 0;
            for (level_t level = 0; level <= node.level(); ++level)
                edges += neighbors_(node, level).size();

            ++result.nodes;
            result.allocated_bytes += node_bytes_(node).size();
            result.edges += edges;
            result.max_edges += max_edges;
        }
        return result;
    }

    /**
     *  @brief  Aggregates stats on the number of nodes, edges, and memory usage up to a specific level.
     *
     *  The `level` parameter is zero-based, where `0` is the base level.
     *  For example, `level=1` will include the base level and the first level of connections.
     */
    stats_t stats(std::size_t level) const noexcept {
        stats_t result{};
        std::size_t neighbors_bytes = !level ? pre_.neighbors_base_bytes : pre_.neighbors_bytes;
        std::size_t max_edges_per_node = !level ? config_.connectivity_base : config_.connectivity;

        for (std::size_t i = 0; i != size(); ++i) {
            node_t node = node_at_(i);
            if (static_cast<std::size_t>(node.level()) < level)
                continue;

            ++result.nodes;
            result.edges += neighbors_(node, static_cast<level_t>(level)).size();
            result.allocated_bytes += node_head_bytes_() + neighbors_bytes;
        }

        result.max_edges = result.nodes * max_edges_per_node;
        return result;
    }

    /**
     *  @brief  Aggregates stats on the number of nodes, edges, and memory usage up to a specific level,
     *          simultaneously exporting the stats for each level into the `stats_per_level` C-style array.
     *
     *  The `max_level` parameter is zero-based, where `0` is the base level.
     *  For example, `max_level=1` will include the base level and the first level of connections.
     */
    stats_t stats(stats_t* stats_per_level, std::size_t max_level) const noexcept {

        std::size_t head_bytes = node_head_bytes_();
        for (std::size_t i = 0; i != size(); ++i) {
            node_t node = node_at_(i);

            stats_per_level[0].nodes++;
            stats_per_level[0].edges += neighbors_(node, 0).size();
            stats_per_level[0].allocated_bytes += pre_.neighbors_base_bytes + head_bytes;

            level_t node_level = static_cast<level_t>(node.level());
            for (level_t l = 1; l <= (std::min)(node_level, static_cast<level_t>(max_level)); ++l) {
                stats_per_level[l].nodes++;
                stats_per_level[l].edges += neighbors_(node, l).size();
                stats_per_level[l].allocated_bytes += pre_.neighbors_bytes;
            }
        }

        // The `max_edges` parameter can be inferred from `nodes`
        stats_per_level[0].max_edges = stats_per_level[0].nodes * config_.connectivity_base;
        for (std::size_t l = 1; l <= max_level; ++l)
            stats_per_level[l].max_edges = stats_per_level[l].nodes * config_.connectivity;

        // Aggregate stats across levels
        stats_t result{};
        for (std::size_t l = 0; l <= max_level; ++l)
            result.nodes += stats_per_level[l].nodes,                         //
                result.edges += stats_per_level[l].edges,                     //
                result.allocated_bytes += stats_per_level[l].allocated_bytes, //
                result.max_edges += stats_per_level[l].max_edges;             //

        return result;
    }

    /**
     *  @brief  A relatively accurate lower bound on the amount of memory consumed by the system.
     *          In practice it's error will be below 10%.
     *
     *  @see    `serialized_length` for the length of the binary serialized representation.
     */
    std::size_t memory_usage(std::size_t allocator_entry_bytes = default_allocator_entry_bytes()) const noexcept {
        std::size_t total = 0;
        if (!viewed_file_) {
            stats_t s = stats();
            total += s.allocated_bytes;
            total += s.nodes * allocator_entry_bytes;
        }

        // Temporary data-structures, proportional to the number of nodes:
        total += limits_.members * sizeof(node_t) + allocator_entry_bytes;

        // Temporary data-structures, proportional to the number of threads:
        total += limits_.threads() * sizeof(context_t) + allocator_entry_bytes * 3;
        return total;
    }

    std::size_t memory_usage_per_node(level_t level) const noexcept { return node_bytes_(level); }

    double inverse_log_connectivity() const { return pre_.inverse_log_connectivity; }

    std::size_t neighbors_base_bytes() const { return pre_.neighbors_base_bytes; }

    std::size_t neighbors_bytes() const { return pre_.neighbors_bytes; }

#if defined(USEARCH_USE_PRAGMA_REGION)
#pragma endregion

#pragma region Serialization
#endif

    /**
     *  @brief  Estimate the binary length (in bytes) of the serialized index.
     */
    std::size_t serialized_length() const noexcept {
        std::size_t neighbors_length = 0;
        for (std::size_t i = 0; i != size(); ++i)
            neighbors_length += node_bytes_(node_at_(i).level()) + sizeof(level_t);
        return sizeof(index_serialized_header_t) + neighbors_length;
    }

    /**
     *  @brief  Saves serialized binary index representation to a stream.
     */
    template <typename output_callback_at, typename progress_at = dummy_progress_t>
    serialization_result_t save_to_stream(output_callback_at&& output, progress_at&& progress = {}) const noexcept {

        serialization_result_t result;

        // Export some basic metadata
        index_serialized_header_t header;
        header.size = nodes_count_;
        header.connectivity = config_.connectivity;
        header.connectivity_base = config_.connectivity_base;
        header.max_level = max_level_;
        header.entry_slot = entry_slot_;
        if (!output(&header, sizeof(header)))
            return result.failed("Failed to serialize the header into stream");

        // Progress status
        std::size_t processed = 0;
        checked_size_result_t header_size = checked_size_from_u64(header.size);
        if (!header_size)
            return result.failed("Index is too large to serialize");
        checked_size_result_t total = checked_mul(std::size_t{2}, header_size.value);
        if (!total)
            return result.failed("Index is too large to serialize");

        // Export the number of levels per node
        // That is both enough to estimate the overall memory consumption,
        // and to be able to estimate the offsets of every entry in the file.
        for (std::size_t i = 0; i != header_size.value; ++i) {
            node_t node = node_at_(i);
            level_t level = node.level();
            if (!output(&level, sizeof(level)))
                return result.failed("Failed to serialize into stream");
            if (!progress(++processed, total.value))
                return result.failed("Terminated by user");
        }

        // After that dump the nodes themselves
        for (std::size_t i = 0; i != header_size.value; ++i) {
            span_bytes_t node_bytes = node_bytes_(node_at_(i));
            if (!output(node_bytes.data(), node_bytes.size()))
                return result.failed("Failed to serialize into stream");
            if (!progress(++processed, total.value))
                return result.failed("Terminated by user");
        }

        return {};
    }

    /**
     *  @brief  Symmetric to `save_from_stream`, pulls data from a stream.
     */
    template <typename input_callback_at, typename progress_at = dummy_progress_t>
    serialization_result_t load_from_stream(input_callback_at&& input, progress_at&& progress = {}) noexcept {

        serialization_result_t result;

        // Remove previously stored objects
        index_limits_t old_limits = limits_;
        reset();

        // Pull basic metadata
        index_serialized_header_t header;
        if (!input(&header, sizeof(header)))
            return result.failed("Failed to pull the header from the stream");

        // We are loading an empty index, no more work to do
        if (!header.size) {
            reset();
            return result;
        }

        // Allocate some dynamic memory to read all the levels
        using levels_allocator_t = typename dynamic_allocator_traits_t::template rebind_alloc<level_t>;
        checked_size_result_t header_size = checked_size_from_u64(header.size);
        if (!header_size)
            return result.failed("Index is too large");
        buffer_gt<level_t, levels_allocator_t> levels(header_size.value);
        if (!levels)
            return result.failed("Out of memory");
        checked_size_result_t levels_bytes = checked_mul(header_size.value, sizeof(level_t));
        if (!levels_bytes)
            return result.failed("Index is too large");
        if (!input(levels, levels_bytes.value))
            return result.failed("Failed to pull nodes levels from the stream");

        // Submit metadata
        config_.connectivity = header.connectivity;
        config_.connectivity_base = header.connectivity_base;
        error_t error = config_.validate();
        if (error)
            return result.failed(std::move(error));

        pre_ = precompute_(config_);
        index_limits_t limits;
        limits.members = header_size.value;
        limits.threads_add = (std::max<std::size_t>)(1, old_limits.threads_add);
        limits.threads_search = (std::max<std::size_t>)(1, old_limits.threads_search);
        if (!reserve(limits)) {
            reset();
            return result.failed("Out of memory");
        }
        nodes_count_ = header_size.value;
        max_level_ = static_cast<level_t>(header.max_level);
        entry_slot_ = static_cast<compressed_slot_t>(header.entry_slot);

        // Load the nodes
        for (std::size_t i = 0; i != header_size.value; ++i) {
            span_bytes_t node_bytes = node_malloc_(levels[i]);
            if (!input(node_bytes.data(), node_bytes.size())) {
                reset();
                return result.failed("Failed to pull nodes from the stream");
            }
            nodes_[i] = node_t{node_bytes.data()};
            if (!progress(i + 1, header_size.value))
                return result.failed("Terminated by user");
        }
        return {};
    }

    template <typename progress_at = dummy_progress_t>
    serialization_result_t save(char const* file_path, progress_at&& progress = {}) const noexcept {
        return save(output_file_t(file_path), std::forward<progress_at>(progress));
    }

    template <typename progress_at = dummy_progress_t>
    serialization_result_t load(char const* file_path, progress_at&& progress = {}) noexcept {
        return load(input_file_t(file_path), std::forward<progress_at>(progress));
    }

    /**
     *  @brief  Saves serialized binary index representation to a file, generally on disk.
     */
    template <typename progress_at = dummy_progress_t>
    serialization_result_t save(output_file_t file, progress_at&& progress = {}) const noexcept {

        serialization_result_t io_result = file.open_if_not();
        if (!io_result)
            return io_result;

        serialization_result_t stream_result = save_to_stream(
            [&](void* buffer, std::size_t length) {
                io_result = file.write(buffer, length);
                return !!io_result;
            },
            std::forward<progress_at>(progress));

        if (!stream_result) {
            // Drop generic messages like "end of file reached" in favor
            // of more specific messages from the stream
            io_result.error.release();
            return stream_result;
        }
        return io_result;
    }

    /**
     *  @brief  Memory-maps the serialized binary index representation from disk,
     *          @b without copying data into RAM, and fetching it on-demand.
     */
    template <typename progress_at = dummy_progress_t>
    serialization_result_t save(memory_mapped_file_t file, std::size_t offset = 0,
                                progress_at&& progress = {}) const noexcept {

        serialization_result_t io_result = file.open_if_not();
        if (!io_result)
            return io_result;

        serialization_result_t stream_result = save_to_stream(
            [&](void* buffer, std::size_t length) {
                if (offset + length > file.size())
                    return false;
                std::memcpy(file.data() + offset, buffer, length);
                offset += length;
                return true;
            },
            std::forward<progress_at>(progress));

        return stream_result;
    }

    /**
     *  @brief  Loads the serialized binary index representation from disk to RAM.
     *          Adjusts the configuration properties of the constructed index to
     *          match the settings in the file.
     */
    template <typename progress_at = dummy_progress_t>
    serialization_result_t load(input_file_t file, progress_at&& progress = {}) noexcept {

        serialization_result_t io_result = file.open_if_not();
        if (!io_result)
            return io_result;

        serialization_result_t stream_result = load_from_stream(
            [&](void* buffer, std::size_t length) {
                io_result = file.read(buffer, length);
                return !!io_result;
            },
            std::forward<progress_at>(progress));

        if (!stream_result) {
            // Drop generic messages like "end of file reached" in favor
            // of more specific messages from the stream
            io_result.error.release();
            return stream_result;
        }
        return io_result;
    }

    /**
     *  @brief  Loads the serialized binary index representation from disk to RAM.
     *          Adjusts the configuration properties of the constructed index to
     *          match the settings in the file.
     */
    template <typename progress_at = dummy_progress_t>
    serialization_result_t load(memory_mapped_file_t file, std::size_t offset = 0,
                                progress_at&& progress = {}) noexcept {

        serialization_result_t io_result = file.open_if_not();
        if (!io_result)
            return io_result;

        serialization_result_t stream_result = load_from_stream(
            [&](void* buffer, std::size_t length) {
                if (offset + length > file.size())
                    return false;
                std::memcpy(buffer, file.data() + offset, length);
                offset += length;
                return true;
            },
            std::forward<progress_at>(progress));

        return stream_result;
    }

    /**
     *  @brief  Memory-maps the serialized binary index representation from disk,
     *          @b without copying data into RAM, and fetching it on-demand.
     */
    template <typename progress_at = dummy_progress_t>
    serialization_result_t view(memory_mapped_file_t file, std::size_t offset = 0,
                                progress_at&& progress = {}) noexcept {

        // Remove previously stored objects
        index_limits_t old_limits = limits_;
        reset();

        serialization_result_t result = file.open_if_not();
        if (!result)
            return result;

        // Pull basic metadata
        index_serialized_header_t header;
        if (file.size() - offset < sizeof(header))
            return result.failed("File is corrupted and lacks a header");
        std::memcpy(&header, file.data() + offset, sizeof(header));

        if (!header.size) {
            reset();
            return result;
        }
        checked_size_result_t header_size = checked_size_from_u64(header.size);
        if (!header_size)
            return result.failed("Index is too large");

        // Precompute offsets of every node, but before that we need to update the configs
        // This could have been done with `std::exclusive_scan`, but it's only available from C++17.
        using offsets_allocator_t = typename dynamic_allocator_traits_t::template rebind_alloc<std::size_t>;
        buffer_gt<std::size_t, offsets_allocator_t> offsets(header_size.value);
        if (!offsets)
            return result.failed("Out of memory");

        config_.connectivity = header.connectivity;
        config_.connectivity_base = header.connectivity_base;
        error_t error = config_.validate();
        if (error)
            return result.failed(std::move(error));

        pre_ = precompute_(config_);
        misaligned_ptr_gt<level_t> levels{(byte_t*)file.data() + offset + sizeof(header)};
        checked_size_result_t levels_bytes = checked_mul(sizeof(level_t), header_size.value);
        checked_size_result_t offset_after_header = checked_add(offset, sizeof(header));
        checked_size_result_t first_offset = levels_bytes && offset_after_header
                                                 ? checked_add(offset_after_header.value, levels_bytes.value)
                                                 : checked_size_overflow();
        if (!first_offset)
            return result.failed("Index is too large");
        if (file.size() < first_offset.value) {
            reset();
            return result.failed("File is corrupted and can't fit the node levels");
        }
        offsets[0u] = first_offset.value;
        for (std::size_t i = 1; i < header_size.value; ++i) {
            checked_size_result_t next_offset = checked_add(offsets[i - 1], node_bytes_(levels[i - 1]));
            if (!next_offset)
                return result.failed("Index is too large");
            offsets[i] = next_offset.value;
        }

        checked_size_result_t total_bytes =
            checked_add(offsets[header_size.value - 1], node_bytes_(levels[header_size.value - 1]));
        if (!total_bytes)
            return result.failed("Index is too large");
        if (file.size() < total_bytes.value) {
            reset();
            return result.failed("File is corrupted and can't fit all the nodes");
        }

        // Submit metadata and reserve memory
        index_limits_t limits;
        limits.members = header_size.value;
        limits.threads_add = (std::max<std::size_t>)(1, old_limits.threads_add);
        limits.threads_search = (std::max<std::size_t>)(1, old_limits.threads_search);
        if (!reserve(limits)) {
            reset();
            return result.failed("Out of memory");
        }
        nodes_count_ = header_size.value;
        max_level_ = static_cast<level_t>(header.max_level);
        entry_slot_ = static_cast<compressed_slot_t>(header.entry_slot);

        // Rapidly address all the nodes
        for (std::size_t i = 0; i != header_size.value; ++i) {
            nodes_[i] = node_t{(byte_t*)file.data() + offsets[i]};
            if (!progress(i + 1, header_size.value))
                return result.failed("Terminated by user");
        }
        viewed_file_ = std::move(file);
        return {};
    }

#if defined(USEARCH_USE_PRAGMA_REGION)
#pragma endregion
#endif

    /**
     *  @brief  Performs compaction on the whole HNSW index, purging some entries
     *          and links to them, while also generating a more efficient mapping,
     *          putting the more frequently used entries closer together.
     *
     *  @param[in] values A []-subscriptable object, providing access to the values.
     *  @param[in] metric Callable object measuring distance between any ::values and present objects.
     *  @param[in] slot_transition Callable object to inform changes in slot assignments.
     *  @param[in] executor Thread-pool to execute the job in parallel.
     *  @param[in] progress Callback to report the execution progress.
     *  @param[in] prefetch Callable object to prefetch data into the cache.
     */
    template <typename values_at, typename metric_at,                   //
              typename slot_transition_at = dummy_key_to_key_mapping_t, //
              typename executor_at = dummy_executor_t,                  //
              typename progress_at = dummy_progress_t,                  //
              typename prefetch_at = dummy_prefetch_t>
    void compact(                             //
        values_at&& values,                   //
        metric_at&& metric,                   //
        slot_transition_at&& slot_transition, //

        executor_at&& executor = executor_at{}, //
        progress_at&& progress = progress_at{}, //
        prefetch_at&& prefetch = prefetch_at{}) noexcept {

        // Export all the keys, slots, and levels.
        // Partition them with the predicate.
        // Sort the allowed entries in descending order of their level.
        // Create a new array mapping old slots to the new ones (INT_MAX for deleted items).
        struct slot_level_t {
            compressed_slot_t old_slot;
            compressed_slot_t cluster;
            level_t level;
        };
        using slot_level_allocator_t = typename dynamic_allocator_traits_t::template rebind_alloc<slot_level_t>;
        buffer_gt<slot_level_t, slot_level_allocator_t> slots_and_levels(size());

        // Progress status
        std::atomic<bool> do_tasks{true};
        std::atomic<std::size_t> processed{0};
        checked_size_result_t total = checked_mul(std::size_t{3}, slots_and_levels.size());
        if (!total)
            return;

        // For every bottom level node, determine its parent cluster
        executor.dynamic(slots_and_levels.size(), [&](std::size_t thread_idx, std::size_t old_slot_as_uint) {
            context_t& context = contexts_[thread_idx];
            compressed_slot_t old_slot = static_cast<compressed_slot_t>(old_slot_as_uint);
            compressed_slot_t cluster = search_for_one_( //
                values[citerator_at(old_slot)],          //
                metric, prefetch,                        //
                static_cast<compressed_slot_t>(entry_slot_), max_level_, 0, context);
            slots_and_levels[old_slot] = {old_slot, cluster, node_at_(old_slot).level()};
            ++processed;
            if (thread_idx == 0)
                do_tasks = progress(processed.load(), total.value);
            return do_tasks.load();
        });
        if (!do_tasks.load())
            return;

        // Where the actual permutation happens:
        std::sort(slots_and_levels.begin(), slots_and_levels.end(), [](slot_level_t const& a, slot_level_t const& b) {
            return a.level == b.level ? a.cluster < b.cluster : a.level > b.level;
        });

        using size_allocator_t = typename dynamic_allocator_traits_t::template rebind_alloc<std::size_t>;
        buffer_gt<std::size_t, size_allocator_t> old_slot_to_new(slots_and_levels.size());
        for (std::size_t new_slot = 0; new_slot != slots_and_levels.size(); ++new_slot)
            old_slot_to_new[slots_and_levels[new_slot].old_slot] = new_slot;

        // Erase all the incoming links
        buffer_gt<node_t, nodes_allocator_t> reordered_nodes(slots_and_levels.size());
        tape_allocator_t reordered_tape;

        for (std::size_t new_slot = 0; new_slot != slots_and_levels.size(); ++new_slot) {
            std::size_t old_slot = slots_and_levels[new_slot].old_slot;
            node_t old_node = node_at_(old_slot);

            std::size_t node_bytes = node_bytes_(old_node.level());
            byte_t* new_data = (byte_t*)reordered_tape.allocate(node_bytes);
            node_t new_node{new_data};
            std::memcpy(new_data, old_node.tape(), node_bytes);

            for (level_t level = 0; level <= old_node.level(); ++level)
                for (misaligned_ref_gt<compressed_slot_t> neighbor : neighbors_(new_node, level))
                    neighbor = static_cast<compressed_slot_t>(old_slot_to_new[compressed_slot_t(neighbor)]);

            reordered_nodes[new_slot] = new_node;
            if (!progress(++processed, total.value))
                return;
        }

        for (std::size_t new_slot = 0; new_slot != slots_and_levels.size(); ++new_slot) {
            std::size_t old_slot = slots_and_levels[new_slot].old_slot;
            slot_transition(node_at_(old_slot).ckey(),                //
                            static_cast<compressed_slot_t>(old_slot), //
                            static_cast<compressed_slot_t>(new_slot));
            if (!progress(++processed, total.value))
                return;
        }

        nodes_ = std::move(reordered_nodes);
        tape_allocator_ = std::move(reordered_tape);
        entry_slot_ = old_slot_to_new[entry_slot_];
    }

    /**
     *  @brief  Scans the whole collection, removing the links leading towards
     *          banned entries. This essentially isolates some nodes from the rest
     *          of the graph, while keeping their outgoing links, in case the node
     *          is structurally relevant and has a crucial role in the index.
     *          It won't reclaim the memory.
     *
     *  @param[in] allow_member Predicate to mark nodes for isolation.
     *  @param[in] executor Thread-pool to execute the job in parallel.
     *  @param[in] progress Callback to report the execution progress.
     */
    template <                                        //
        typename allow_member_at = dummy_predicate_t, //
        typename executor_at = dummy_executor_t,      //
        typename progress_at = dummy_progress_t       //
        >
    void isolate(                               //
        allow_member_at&& allow_member,         //
        executor_at&& executor = executor_at{}, //
        progress_at&& progress = progress_at{}) noexcept {

        // Progress status
        std::atomic<bool> do_tasks{true};
        std::atomic<std::size_t> processed{0};

        // Erase all the incoming links
        std::size_t nodes_count = size();
        executor.dynamic(nodes_count, [&](std::size_t thread_idx, std::size_t node_idx) {
            node_t node = node_at_(node_idx);
            for (level_t level = 0; level <= node.level(); ++level) {
                neighbors_ref_t neighbors = neighbors_(node, level);
                neighbors.erase_if([&](compressed_slot_t neighbor_slot) {
                    node_t neighbor = node_at_(neighbor_slot);
                    return !allow_member(member_cref_t{neighbor.ckey(), neighbor_slot});
                });
            }
            ++processed;
            if (thread_idx == 0)
                do_tasks = progress(processed.load(), nodes_count);
            return do_tasks.load();
        });

        // At the end report the latest numbers, because the reporter thread may be finished earlier
        progress(processed.load(), nodes_count);
    }

  private:
    inline static precomputed_constants_t precompute_(index_config_t const& config) noexcept {
        precomputed_constants_t pre;
        pre.inverse_log_connectivity = 1.0 / std::log(static_cast<double>(config.connectivity));
        pre.neighbors_bytes = config.connectivity * sizeof(compressed_slot_t) + sizeof(neighbors_count_t);
        pre.neighbors_base_bytes = config.connectivity_base * sizeof(compressed_slot_t) + sizeof(neighbors_count_t);
        return pre;
    }

    using span_bytes_t = span_gt<byte_t>;

    inline span_bytes_t node_bytes_(node_t node) const noexcept { return {node.tape(), node_bytes_(node.level())}; }
    inline std::size_t node_bytes_(level_t level) const noexcept {
        return node_head_bytes_() + node_neighbors_bytes_(level);
    }
    inline std::size_t node_neighbors_bytes_(node_t node) const noexcept { return node_neighbors_bytes_(node.level()); }
    inline std::size_t node_neighbors_bytes_(level_t level) const noexcept {
        return pre_.neighbors_base_bytes + pre_.neighbors_bytes * level;
    }

    span_bytes_t node_malloc_(level_t level) noexcept {
        std::size_t node_bytes = node_bytes_(level);
        byte_t* data = (byte_t*)tape_allocator_.allocate(node_bytes);
        return data ? span_bytes_t{data, node_bytes} : span_bytes_t{};
    }

    node_t node_make_(vector_key_t key, level_t level) noexcept {
        span_bytes_t node_bytes = node_malloc_(level);
        if (!node_bytes)
            return {};

        std::memset(node_bytes.data(), 0, node_bytes.size());
        node_t node{(byte_t*)node_bytes.data()};
        node.key(key);
        node.level(level);
        return node;
    }

    node_t node_make_copy_(span_bytes_t old_bytes) noexcept {
        byte_t* data = (byte_t*)tape_allocator_.allocate(old_bytes.size());
        if (!data)
            return {};
        std::memcpy(data, old_bytes.data(), old_bytes.size());
        return node_t{data};
    }

    void node_free_(std::size_t idx) noexcept {
        if (viewed_file_)
            return;

        node_t& node = nodes_[idx];
        tape_allocator_.deallocate(node.tape(), node_bytes_(node).size());
        node = node_t{};
    }

    /**
     *  从邻居列表中摘掉指向 `banned` 的边（0..max_level 各层）。
     *  用于 insert 中途 OOM：高层已 form_reverse_links_ 时避免悬空反向边。
     *  只遍历 banned 的出边（即曾接收反向链接的节点），避免全图 isolate。
     */
    void unlink_slot_(compressed_slot_t banned, level_t max_level) noexcept {
        node_t banned_node = node_at_(banned);
        // level() 返回 misaligned_ref，须先落到标量再与 max_level 比较。
        level_t const banned_level = static_cast<level_t>(banned_node.level());
        level_t const top = (std::min)(max_level, banned_level);
        for (level_t level = 0; level <= top; ++level) {
            neighbors_ref_t outs = neighbors_(banned_node, level);
            // 拷贝出边再改邻居，避免持锁遍历时列表被改写。
            std::size_t const n = outs.size();
            for (std::size_t i = 0; i != n; ++i) {
                compressed_slot_t neigh = outs[i];
                if (neigh == banned)
                    continue;
                node_lock_t lock = node_lock_(neigh);
                neighbors_(node_at_(neigh), level).erase_if([banned](compressed_slot_t s) { return s == banned; });
            }
            {
                node_lock_t self = node_lock_(banned);
                neighbors_(node_at_(banned), level).clear();
            }
        }
    }

    inline node_t node_at_(std::size_t idx) const noexcept { return nodes_[idx]; }
    inline neighbors_ref_t neighbors_base_(node_t node) const noexcept { return {node.neighbors_tape()}; }

    inline neighbors_ref_t neighbors_non_base_(node_t node, level_t level) const noexcept {
        usearch_assert_m(level > 0 && level <= node.level(), "Linking to missing level");
        return {node.neighbors_tape() + pre_.neighbors_base_bytes + (level - 1) * pre_.neighbors_bytes};
    }

    inline neighbors_ref_t neighbors_(node_t node, level_t level) const noexcept {
        return level ? neighbors_non_base_(node, level) : neighbors_base_(node);
    }

    struct node_lock_t {
        nodes_mutexes_t& mutexes;
        std::size_t slot;
        inline ~node_lock_t() noexcept { mutexes.unlock(slot); }
    };

    inline node_lock_t node_lock_(std::size_t slot) const noexcept {
        nodes_mutexes_.lock(slot);
        return {nodes_mutexes_, slot};
    }

    struct optional_node_lock_t {
        nodes_mutexes_t& mutexes;
        std::size_t slot;
        inline ~optional_node_lock_t() noexcept {
            if (slot != (std::numeric_limits<std::size_t>::max)())
                mutexes.unlock(slot);
        }
    };

    inline optional_node_lock_t optional_node_lock_(std::size_t slot, bool condition) const noexcept {
        if (condition) {
            nodes_mutexes_.lock(slot);
            return {nodes_mutexes_, slot};
        } else {
            return {nodes_mutexes_, (std::numeric_limits<std::size_t>::max)()};
        }
    }

    struct node_conditional_lock_t {
        nodes_mutexes_t& mutexes;
        std::size_t slot;
        inline ~node_conditional_lock_t() noexcept {
            if (slot != (std::numeric_limits<std::size_t>::max)())
                mutexes.unlock(slot);
        }
    };

    inline node_conditional_lock_t node_try_conditional_lock_(std::size_t slot, bool condition,
                                                              bool& failed_to_acquire) const noexcept {
        if (!condition) {
            failed_to_acquire = false;
            return {nodes_mutexes_, (std::numeric_limits<std::size_t>::max)()};
        }
        failed_to_acquire = nodes_mutexes_.atomic_set(slot);
        return {nodes_mutexes_, failed_to_acquire ? (std::numeric_limits<std::size_t>::max)() : slot};
    }

    template <typename metric_at, bool require_non_empty_ak = false>
    candidates_view_t form_links_to_closest_( //
        metric_at&& metric, std::size_t new_slot, level_t level, context_t& context) usearch_noexcept_m {

        node_t new_node = node_at_(new_slot);
        top_candidates_t& top = context.top_candidates;
        usearch_assert_m(top.size() || !require_non_empty_ak, "No candidates found");
        candidates_view_t top_view =
            refine_(metric, config_.connectivity, top, context, context.computed_distances_in_refines);
        usearch_assert_m(top_view.size() || !require_non_empty_ak, "This would lead to isolated nodes");

        // Outgoing links from `new_slot`:
        neighbors_ref_t new_neighbors = neighbors_(new_node, level);
        usearch_assert_m(!new_neighbors.size(), "The newly inserted element should have blank link list");
        for (std::size_t idx = 0; idx != top_view.size(); idx++) {
            usearch_assert_m(!new_neighbors[idx], "Possible memory corruption");
            usearch_assert_m(level <= node_at_(top_view[idx].slot).level(), "Linking to missing level");
            new_neighbors.push_back(top_view[idx].slot);
        }

        return top_view;
    }

    template <typename value_at, typename metric_at>
    void form_reverse_links_( //
        metric_at&& metric, compressed_slot_t new_slot, candidates_view_t new_neighbors, value_at&& value,
        level_t level, context_t& context) usearch_noexcept_m {

        top_candidates_t& top_for_refine = context.top_for_refine;
        std::size_t const connectivity_max = level ? config_.connectivity : config_.connectivity_base;

        // Reverse links from the neighbors:
        for (auto new_neighbor : new_neighbors) {
            compressed_slot_t close_slot = new_neighbor.slot;
            if (close_slot == new_slot)
                continue;
            node_lock_t close_lock = node_lock_(close_slot);
            node_t close_node = node_at_(close_slot);
            neighbors_ref_t close_header = neighbors_(close_node, level);

            // The node may have no neighbors only in one case, when it's the first one in the index,
            // but that is problematic to track in multi-threaded environments, where the order of insertion
            // is not guaranteed.
            // usearch_assert_m(close_header.size() || new_slot == 1, "Possible corruption - isolated node");
            usearch_assert_m(close_header.size() <= connectivity_max, "Possible corruption - overflow");
            usearch_assert_m(close_slot != new_slot, "Self-loops are impossible");
            usearch_assert_m(level <= close_node.level(), "Linking to missing level");

            // Skip to prevent duplicate entries in the neighbor list.
            if (std::find_if(close_header.begin(), close_header.end(),
                             [new_slot](compressed_slot_t slot) { return slot == new_slot; }) != close_header.end()) {
                continue;
            }

            if (close_header.size() < connectivity_max) {
                close_header.push_back(new_slot);
                continue;
            }

            top_for_refine.clear();
            top_for_refine.insert_reserved({context.measure(value, citerator_at(close_slot), metric), new_slot});
            for (compressed_slot_t successor_slot : close_header)
                top_for_refine.insert_reserved(
                    {context.measure(citerator_at(close_slot), citerator_at(successor_slot), metric), successor_slot});

            // Export the results:
            close_header.clear();
            candidates_view_t top_view = refine_(metric, connectivity_max, top_for_refine, context,
                                                 context.computed_distances_in_reverse_refines, new_slot, value);
            usearch_assert_m(top_view.size(), "This would lead to isolated nodes");
            for (std::size_t idx = 0; idx != top_view.size(); idx++)
                close_header.push_back(top_view[idx].slot);
        }
    }

    level_t choose_random_level_(std::default_random_engine& level_generator) const noexcept {
        std::uniform_real_distribution<double> distribution(0.0, 1.0);
        double r = -std::log(distribution(level_generator)) * pre_.inverse_log_connectivity;
        return (level_t)r;
    }

    struct candidates_range_t;
    class candidates_iterator_t {
        friend struct candidates_range_t;

        index_gt const& index_;
        neighbors_ref_t neighbors_;
        visits_hash_set_t& visits_;
        std::size_t current_;

        candidates_iterator_t& skip_missing() noexcept {
            if (!visits_.size())
                return *this;
            while (current_ != neighbors_.size()) {
                compressed_slot_t neighbor_slot = neighbors_[current_];
                if (visits_.test(neighbor_slot))
                    current_++;
                else
                    break;
            }
            return *this;
        }

      public:
        using element_t = compressed_slot_t;
        using iterator_category = std::forward_iterator_tag;
        using value_type = element_t;
        using difference_type = std::ptrdiff_t;
        using pointer = misaligned_ptr_gt<element_t>;
        using reference = misaligned_ref_gt<element_t>;

        value_type operator*() const noexcept { return neighbors_[current_]; }
        candidates_iterator_t(index_gt const& index, neighbors_ref_t neighbors, visits_hash_set_t& visits,
                              std::size_t progress) noexcept
            : index_(index), neighbors_(neighbors), visits_(visits), current_(progress) {}
        candidates_iterator_t operator++(int) noexcept {
            candidates_iterator_t old(index_, neighbors_, visits_, current_);
            ++(*this);
            return old;
        }
        candidates_iterator_t& operator++() noexcept {
            ++current_;
            skip_missing();
            return *this;
        }
        bool operator==(candidates_iterator_t const& other) noexcept { return current_ == other.current_; }
        bool operator!=(candidates_iterator_t const& other) noexcept { return current_ != other.current_; }

        vector_key_t key() const noexcept { return index_.node_at_(slot()).key(); }
        compressed_slot_t slot() const noexcept { return neighbors_[current_]; }
        friend inline std::size_t get_slot(candidates_iterator_t const& it) noexcept { return it.slot(); }
        friend inline vector_key_t get_key(candidates_iterator_t const& it) noexcept { return it.key(); }
    };

    struct candidates_range_t {
        index_gt const& index;
        neighbors_ref_t neighbors;
        visits_hash_set_t& visits;

        candidates_iterator_t begin() const noexcept {
            return candidates_iterator_t{index, neighbors, visits, 0}.skip_missing();
        }
        candidates_iterator_t end() const noexcept { return {index, neighbors, visits, neighbors.size()}; }
    };

    template <typename value_at, typename metric_at, typename prefetch_at = dummy_prefetch_t>
    compressed_slot_t search_for_one_(                                //
        value_at&& query, metric_at&& metric, prefetch_at&& prefetch, //
        compressed_slot_t closest_slot, level_t begin_level, level_t end_level, context_t& context) const noexcept {

        visits_hash_set_t& visits = context.visits;
        visits.clear();

        // Optional prefetching
        if (!is_dummy<prefetch_at>())
            prefetch(citerator_at(closest_slot), citerator_at(closest_slot) + 1);

        bool const need_lock = !is_immutable();
        distance_t closest_dist = context.measure(query, citerator_at(closest_slot), metric);
        for (level_t level = begin_level; level > end_level; --level) {
            bool changed;
            do {
                changed = false;
                optional_node_lock_t closest_lock = optional_node_lock_(closest_slot, need_lock);
                neighbors_ref_t closest_neighbors = neighbors_non_base_(node_at_(closest_slot), level);

                // Optional prefetching
                if (!is_dummy<prefetch_at>()) {
                    candidates_range_t missing_candidates{*this, closest_neighbors, visits};
                    prefetch(missing_candidates.begin(), missing_candidates.end());
                }

                // Actual traversal
                for (compressed_slot_t candidate_slot : closest_neighbors) {
                    distance_t candidate_dist = context.measure(query, citerator_at(candidate_slot), metric);
                    if (candidate_dist < closest_dist) {
                        closest_dist = candidate_dist;
                        closest_slot = candidate_slot;
                        changed = true;
                    }
                }

                context.iteration_cycles++;
            } while (changed);
        }
        return closest_slot;
    }

    /**
     *  @brief  Traverses a layer of a graph, to find the best place to insert a new node.
     *          Locks the nodes in the process, assuming other threads are updating neighbors lists.
     *  @return `true` if procedure succeeded, `false` if run out of memory.
     */
    template <typename value_at, typename metric_at, typename prefetch_at = dummy_prefetch_t>
    bool search_to_insert_(                                           //
        value_at&& query, metric_at&& metric, prefetch_at&& prefetch, //
        compressed_slot_t start_slot, level_t level, std::size_t top_limit, context_t& context) noexcept {

        visits_hash_set_t& visits = context.visits;
        next_candidates_t& next = context.next_candidates; // pop min, push
        top_candidates_t& top = context.top_candidates;    // pop max, push

        visits.clear();
        next.clear();
        top.clear();

        // At the very least we are going to explore the starting node and its neighbors
        if (!visits.reserve(config_.connectivity_base + 1u))
            return false;
        if (!top.reserve(top_limit))
            return false;
        if (!next.reserve(top_limit))
            return false;

        // Optional prefetching
        if (!is_dummy<prefetch_at>())
            prefetch(citerator_at(start_slot), citerator_at(start_slot) + 1);

        distance_t radius = context.measure(query, citerator_at(start_slot), metric);
        next.insert_reserved({-radius, start_slot});
        top.insert_reserved({radius, start_slot});
        visits.set(start_slot);

        // The primary loop of the graph traversal
        while (!next.empty()) {

            candidate_t candidacy = next.top();
            if ((-candidacy.distance) > radius && top.size() == top_limit)
                break;

            next.pop();
            context.iteration_cycles++;

            compressed_slot_t candidate_slot = candidacy.slot;
            node_t candidate_ref = node_at_(candidate_slot);
            node_lock_t candidate_lock = node_lock_(candidate_slot);
            neighbors_ref_t candidate_neighbors = neighbors_(candidate_ref, level);

            // Optional prefetching
            if (!is_dummy<prefetch_at>()) {
                candidates_range_t missing_candidates{*this, candidate_neighbors, visits};
                prefetch(missing_candidates.begin(), missing_candidates.end());
            }

            // Assume the worst-case when reserving memory
            if (!visits.reserve(visits.size() + candidate_neighbors.size()))
                return false;

            // 测距当前邻居时预取下一个未访问槽（与 base 检索同一软件流水）
            std::size_t const neigh_n = candidate_neighbors.size();
            for (std::size_t ni = 0; ni != neigh_n; ++ni) {
                compressed_slot_t successor_slot = candidate_neighbors[ni];
                if (visits.set(successor_slot))
                    continue;

                if (!is_dummy<prefetch_at>()) {
                    for (std::size_t nj = ni + 1; nj != neigh_n; ++nj) {
                        compressed_slot_t nxt = candidate_neighbors[nj];
                        if (!visits.contains(nxt)) {
                            prefetch(citerator_at(nxt), citerator_at(nxt) + 1);
                            break;
                        }
                    }
                }

                // We don't access the neighbors of the `successor_slot` node,
                // so we don't have to lock it.
                // node_lock_t successor_lock = node_lock_(successor_slot);
                distance_t successor_dist = context.measure(query, citerator_at(successor_slot), metric);
                if (top.size() < top_limit || successor_dist < radius) {
                    // This can substantially grow our priority queue:
                    next.insert({-successor_dist, successor_slot});
                    // This will automatically evict poor matches:
                    top.insert({successor_dist, successor_slot}, top_limit);
                    radius = top.top().distance;
                }
            }
        }
        return true;
    }

    /**
     *  @brief  Traverses a layer of a graph, to find the best neighbors list for updated node.
     *          Locks the nodes in the process, assuming other threads are updating neighbors lists.
     *  @return `true` if procedure succeeded, `false` if run out of memory.
     */
    template <typename value_at, typename metric_at, typename prefetch_at = dummy_prefetch_t>
    bool search_to_update_(                                           //
        value_at&& query, metric_at&& metric, prefetch_at&& prefetch, //
        compressed_slot_t start_slot, compressed_slot_t updated_slot, level_t level, std::size_t top_limit,
        context_t& context) noexcept {

        visits_hash_set_t& visits = context.visits;
        next_candidates_t& next = context.next_candidates; // pop min, push
        top_candidates_t& top = context.top_candidates;    // pop max, push

        visits.clear();
        next.clear();
        top.clear();

        // At the very least we are going to explore the starting node and its neighbors
        if (!visits.reserve(config_.connectivity_base + 1u))
            return false;
        if (!top.reserve(top_limit))
            return false;
        if (!next.reserve(top_limit))
            return false;

        // Optional prefetching
        if (!is_dummy<prefetch_at>())
            prefetch(citerator_at(start_slot), citerator_at(start_slot) + 1);

        distance_t radius = context.measure(query, citerator_at(start_slot), metric);
        next.insert_reserved({-radius, start_slot});
        visits.set(start_slot);
        if (start_slot != updated_slot)
            top.insert_reserved({radius, start_slot});

        // The primary loop of the graph traversal
        while (!next.empty()) {

            candidate_t candidacy = next.top();
            if ((-candidacy.distance) > radius && top.size() == top_limit)
                break;

            next.pop();
            context.iteration_cycles++;

            compressed_slot_t candidate_slot = candidacy.slot;
            node_t candidate_ref = node_at_(candidate_slot);

            // The trickiest part of update-heavy workloads is mitigating dead-locks
            // in connected nodes during traversal. A "good enough" solution would be
            // to skip concurrent access, assuming the other "close" node is gonna add
            // this one when forming reverse connections.
            bool failed_to_acquire = false;
            node_conditional_lock_t candidate_lock =
                node_try_conditional_lock_(candidate_slot, updated_slot != candidate_slot, failed_to_acquire);
            if (failed_to_acquire)
                continue;
            auto optional_node_lock = optional_node_lock_(candidate_slot, updated_slot == candidate_slot);
            neighbors_ref_t candidate_neighbors = neighbors_(candidate_ref, level);

            // Optional prefetching
            if (!is_dummy<prefetch_at>()) {
                candidates_range_t missing_candidates{*this, candidate_neighbors, visits};
                prefetch(missing_candidates.begin(), missing_candidates.end());
            }

            // Assume the worst-case when reserving memory
            if (!visits.reserve(visits.size() + candidate_neighbors.size()))
                return false;

            std::size_t const neigh_n = candidate_neighbors.size();
            for (std::size_t ni = 0; ni != neigh_n; ++ni) {
                compressed_slot_t successor_slot = candidate_neighbors[ni];
                if (visits.set(successor_slot))
                    continue;

                if (!is_dummy<prefetch_at>()) {
                    for (std::size_t nj = ni + 1; nj != neigh_n; ++nj) {
                        compressed_slot_t nxt = candidate_neighbors[nj];
                        if (!visits.contains(nxt)) {
                            prefetch(citerator_at(nxt), citerator_at(nxt) + 1);
                            break;
                        }
                    }
                }

                // We don't access the neighbors of the `successor_slot` node,
                // so we don't have to lock it.
                // node_conditional_lock_t successor_lock =
                //     node_try_conditional_lock_(successor_slot, updated_slot != successor_slot);
                distance_t successor_dist = context.measure(query, citerator_at(successor_slot), metric);
                if (top.size() < top_limit || successor_dist < radius) {
                    // This can substantially grow our priority queue:
                    next.insert({-successor_dist, successor_slot});
                    // This will automatically evict poor matches:
                    if (updated_slot != successor_slot)
                        top.insert({successor_dist, successor_slot}, top_limit);
                    radius = top.top().distance;
                }
            }
        }
        return true;
    }

    /**
     *  @brief  Traverses the @b base layer of a graph, to find a close match.
     *          Doesn't lock any nodes, assuming read-only simultaneous access.
     *  @return `true` if procedure succeeded, `false` if run out of memory.
     */
    template <typename value_at, typename metric_at, typename predicate_at, typename prefetch_at>
    bool search_to_find_in_base_(                                                               //
        value_at&& query, metric_at&& metric, predicate_at&& predicate, prefetch_at&& prefetch, //
        compressed_slot_t start_slot, std::size_t expansion, context_t& context) const usearch_noexcept_m {

        visits_hash_set_t& visits = context.visits;
        next_candidates_t& next = context.next_candidates; // pop min, push
        top_candidates_t& top = context.top_candidates;    // pop max, push
        std::size_t const top_limit = expansion;

        visits.clear();
        next.clear();
        top.clear();
        if (!visits.reserve(config_.connectivity_base + 1u))
            return false;

        // Optional prefetching
        if (!is_dummy<prefetch_at>())
            prefetch(citerator_at(start_slot), citerator_at(start_slot) + 1);

        distance_t radius = context.measure(query, citerator_at(start_slot), metric);
        usearch_assert_m(next.capacity(), "The `max_heap_gt` must have been reserved in the search entry point");
        next.insert_reserved({-radius, start_slot});
        visits.set(start_slot);

        // Don't populate the top list if the predicate is not satisfied
        if (is_dummy<predicate_at>() || predicate(member_cref_t{node_at_(start_slot).ckey(), start_slot})) {
            usearch_assert_m(top.capacity(),
                             "The `sorted_buffer_gt` must have been reserved in the search entry point");
            top.insert_reserved({radius, start_slot});
        }

        while (!next.empty()) {

            candidate_t candidate = next.top();
            if ((-candidate.distance) > radius && top.size() == top_limit)
                break;

            next.pop();
            context.iteration_cycles++;

            neighbors_ref_t candidate_neighbors = neighbors_base_(node_at_(candidate.slot));

            // Optional prefetching
            if (!is_dummy<prefetch_at>()) {
                candidates_range_t missing_candidates{*this, candidate_neighbors, visits};
                prefetch(missing_candidates.begin(), missing_candidates.end());
            }

            // Assume the worst-case when reserving memory
            if (!visits.reserve(visits.size() + candidate_neighbors.size()))
                return false;

            // 软件流水：测当前邻居时，用 prefetch 拉下一个未访问邻居（dense 侧预取向量）
            std::size_t const neigh_n = candidate_neighbors.size();
            for (std::size_t ni = 0; ni != neigh_n; ++ni) {
                compressed_slot_t successor_slot = candidate_neighbors[ni];
                if (visits.set(successor_slot))
                    continue;

                if (!is_dummy<prefetch_at>()) {
                    for (std::size_t nj = ni + 1; nj != neigh_n; ++nj) {
                        compressed_slot_t nxt = candidate_neighbors[nj];
                        if (!visits.contains(nxt)) {
                            prefetch(citerator_at(nxt), citerator_at(nxt) + 1);
                            break;
                        }
                    }
                }

                distance_t successor_dist = context.measure(query, citerator_at(successor_slot), metric);
                if (top.size() < top_limit || successor_dist < radius) {
                    // This can substantially grow our priority queue:
                    next.insert({-successor_dist, successor_slot});
                    if (is_dummy<predicate_at>() ||
                        predicate(member_cref_t{node_at_(successor_slot).ckey(), successor_slot})) {
                        top.insert({successor_dist, successor_slot}, top_limit);
                        radius = top.top().distance;
                    }
                }
            }
        }

        return true;
    }

    /**
     *  @brief  Iterates through all members, without actually touching the index.
     */
    template <typename value_at, typename metric_at, typename predicate_at>
    void search_exact_(                                                 //
        value_at&& query, metric_at&& metric, predicate_at&& predicate, //
        std::size_t count, context_t& context) const noexcept {

        top_candidates_t& top = context.top_candidates;
        top.clear();
        top.reserve(count);
        for (std::size_t i = 0; i != size(); ++i) {
            auto slot = static_cast<compressed_slot_t>(i);
            if (!is_dummy<predicate_at>())
                if (!predicate(at(slot)))
                    continue;

            distance_t distance = context.measure(query, citerator_at(slot), metric);
            top.insert(candidate_t{distance, slot}, count);
        }
    }

    /// @brief  Helper for `refine_()`: computes inter-neighbor distance, substituting
    ///         @p override_value when either slot matches @p override_slot.
    ///         The `std::nullptr_t` overload below avoids instantiating the override
    ///         branch when no override is provided, keeping the code C++11 compatible.
    template <typename metric_at, typename override_value_at>
    distance_t inter_neighbor_distance_(                                   //
        candidate_t const& candidate, candidate_t const& submitted,        //
        compressed_slot_t override_slot, override_value_at override_value, //
        metric_at&& metric, context_t& context) const noexcept {
        if (candidate.slot == override_slot)
            return context.measure(override_value, citerator_at(submitted.slot), metric);
        else if (submitted.slot == override_slot)
            return context.measure(override_value, citerator_at(candidate.slot), metric);
        else
            return context.measure(citerator_at(candidate.slot), citerator_at(submitted.slot), metric);
    }

    template <typename metric_at>
    distance_t inter_neighbor_distance_(                            //
        candidate_t const& candidate, candidate_t const& submitted, //
        compressed_slot_t, std::nullptr_t,                          //
        metric_at&& metric, context_t& context) const noexcept {
        return context.measure(citerator_at(candidate.slot), citerator_at(submitted.slot), metric);
    }

    /**
     *  @brief  This algorithm from the original paper implements a heuristic,
     *          that massively reduces the number of connections a point has,
     *          to keep only the neighbors, that are from each other.
     *
     *  @param[in] override_slot  Optional slot whose stored vector is stale (e.g. during update,
     *                            where the callback has not yet committed the new vector).
     *                            When set, inter-result distances involving this slot will use
     *                            @p override_value instead of reading from `citerator_at()`.
     *  @param[in] override_value The up-to-date vector for @p override_slot. Only used when
     *                            @p override_value_at is not `std::nullptr_t`.
     */
    template <typename metric_at, typename override_value_at = std::nullptr_t>
    candidates_view_t refine_(                                         //
        metric_at&& metric,                                            //
        std::size_t needed, top_candidates_t& top, context_t& context, //
        std::size_t& refines_counter,                                  //
        compressed_slot_t override_slot = ((std::numeric_limits<compressed_slot_t>::max))(),
        override_value_at override_value = {}) const noexcept {

        // Avoid expensive computation, if the set is already small
        candidate_t* top_data = top.data();
        std::size_t const top_count = top.size();
        if (top_count < needed)
            return {top_data, top_count};

        // Sort before processing
        top.sort_ascending();

        std::size_t submitted_count = 1;
        std::size_t consumed_count = 1; /// Always equal or greater than `submitted_count`.
        while (submitted_count < needed && consumed_count < top_count) {
            candidate_t candidate = top_data[consumed_count];
            bool good = true;
            std::size_t idx = 0;
            for (; idx < submitted_count; idx++) {
                candidate_t submitted = top_data[idx];
                distance_t inter_result_dist = inter_neighbor_distance_( //
                    candidate, submitted, override_slot, override_value, metric, context);
                if (inter_result_dist < candidate.distance) {
                    good = false;
                    break;
                }
            }
            refines_counter += idx;

            if (good) {
                top_data[submitted_count] = top_data[consumed_count];
                submitted_count++;
            }
            consumed_count++;
        }

        top.shrink(submitted_count);
        return {top_data, submitted_count};
    }
};

struct join_result_t {
    error_t error{};
    std::size_t intersection_size{};
    std::size_t engagements{};
    std::size_t visited_members{};
    std::size_t computed_distances{};

    explicit operator bool() const noexcept { return !error; }
    join_result_t failed(error_t message) noexcept {
        error = std::move(message);
        return std::move(*this);
    }
};

/**
 *  @brief  Adapts the Male-Optimal Stable Marriage algorithm for unequal sets
 *          to perform fast one-to-one matching between two large collections
 *          of vectors, using approximate nearest neighbors search.
 *
 *  @param[inout] man_to_woman Container to map ::men keys to ::women.
 *  @param[inout] woman_to_man Container to map ::women keys to ::men.
 *  @param[in] executor Thread-pool to execute the job in parallel.
 *  @param[in] progress Callback to report the execution progress.
 */
template < //

    typename men_at,          //
    typename women_at,        //
    typename men_values_at,   //
    typename women_values_at, //
    typename men_metric_at,   //
    typename women_metric_at, //

    typename man_to_woman_at = dummy_key_to_key_mapping_t, //
    typename woman_to_man_at = dummy_key_to_key_mapping_t, //
    typename executor_at = dummy_executor_t,               //
    typename progress_at = dummy_progress_t                //
    >
static join_result_t join(               //
    men_at const& men,                   //
    women_at const& women,               //
    men_values_at const& men_values,     //
    women_values_at const& women_values, //
    men_metric_at&& men_metric,          //
    women_metric_at&& women_metric,      //

    index_join_config_t config = {},                    //
    man_to_woman_at&& man_to_woman = man_to_woman_at{}, //
    woman_to_man_at&& woman_to_man = woman_to_man_at{}, //
    executor_at&& executor = executor_at{},             //
    progress_at&& progress = progress_at{}) noexcept {

    if (women.size() < men.size())
        return unum::usearch::join(                                                               //
            women, men,                                                                           //
            women_values, men_values,                                                             //
            std::forward<women_metric_at>(women_metric), std::forward<men_metric_at>(men_metric), //

            config,                                      //
            std::forward<woman_to_man_at>(woman_to_man), //
            std::forward<man_to_woman_at>(man_to_woman), //
            std::forward<executor_at>(executor),         //
            std::forward<progress_at>(progress));

    join_result_t result;

    // Sanity checks and argument validation:
    if (&men == &women)
        return result.failed("Can't join with itself, consider copying");

    if (config.max_proposals == 0)
        config.max_proposals = static_cast<std::size_t>(std::log(men.size())) + executor.size();

    using proposals_count_t = std::uint16_t;
    config.max_proposals = (std::min)(men.size(), config.max_proposals);

    using distance_t = typename men_at::distance_t;
    using dynamic_allocator_traits_t = typename men_at::dynamic_allocator_traits_t;
    using man_key_t = typename men_at::vector_key_t;
    using woman_key_t = typename women_at::vector_key_t;

    // Use the `compressed_slot_t` type of the larger collection
    using compressed_slot_t = typename women_at::compressed_slot_t;
    using compressed_slot_allocator_t = typename dynamic_allocator_traits_t::template rebind_alloc<compressed_slot_t>;
    using proposals_count_allocator_t = typename dynamic_allocator_traits_t::template rebind_alloc<proposals_count_t>;

    // Create an atomic queue, as a ring structure, from/to which
    // free men will be added/pulled.
    std::mutex free_men_mutex{};
    ring_gt<compressed_slot_t, compressed_slot_allocator_t> free_men;
    free_men.reserve(men.size());
    for (std::size_t i = 0; i != men.size(); ++i)
        free_men.push(static_cast<compressed_slot_t>(i));

    // We are gonna need some temporary memory.
    buffer_gt<proposals_count_t, proposals_count_allocator_t> proposal_counts(men.size());
    buffer_gt<compressed_slot_t, compressed_slot_allocator_t> man_to_woman_slots(men.size());
    buffer_gt<compressed_slot_t, compressed_slot_allocator_t> woman_to_man_slots(women.size());
    if (!proposal_counts || !man_to_woman_slots || !woman_to_man_slots)
        return result.failed("Can't temporary mappings");

    compressed_slot_t missing_slot;
    std::memset((void*)&missing_slot, 0xFF, sizeof(compressed_slot_t));
    std::memset((void*)man_to_woman_slots.data(), 0xFF, sizeof(compressed_slot_t) * men.size());
    std::memset((void*)woman_to_man_slots.data(), 0xFF, sizeof(compressed_slot_t) * women.size());
    std::memset(proposal_counts.data(), 0, sizeof(proposals_count_t) * men.size());

    // Define locks, to limit concurrent accesses to `man_to_woman_slots` and `woman_to_man_slots`.
    bitset_t men_locks(men.size()), women_locks(women.size());
    if (!men_locks || !women_locks)
        return result.failed("Can't allocate locks");

    std::atomic<std::size_t> rounds{0};
    std::atomic<std::size_t> engagements{0};
    std::atomic<std::size_t> computed_distances{0};
    std::atomic<std::size_t> visited_members{0};
    std::atomic<char const*> atomic_error{nullptr};

    // Concurrently process all the men
    executor.parallel([&](std::size_t thread_idx) {
        index_search_config_t search_config;
        search_config.expansion = config.expansion;
        search_config.exact = config.exact;
        search_config.thread = thread_idx;
        compressed_slot_t free_man_slot;

        // While there exist a free man who still has a woman to propose to.
        while (!atomic_error.load(std::memory_order_relaxed)) {
            std::size_t passed_rounds = 0;
            std::size_t total_rounds = 0;
            {
                std::unique_lock<std::mutex> pop_lock(free_men_mutex);
                if (!free_men.try_pop(free_man_slot))
                    // Primary exit path, we have exhausted the list of candidates
                    break;
                passed_rounds = ++rounds;
                total_rounds = passed_rounds + free_men.size();
            }
            if (thread_idx == 0 && !progress(passed_rounds, total_rounds)) {
                atomic_error.store("Terminated by user");
                break;
            }
            while (men_locks.atomic_set(free_man_slot))
                ;

            proposals_count_t& free_man_proposals = proposal_counts[free_man_slot];
            if (free_man_proposals >= config.max_proposals)
                continue;

            // Find the closest woman, to whom this man hasn't proposed yet.
            ++free_man_proposals;
            auto candidates = women.search(men_values[free_man_slot], free_man_proposals, women_metric, search_config);
            visited_members += candidates.visited_members;
            computed_distances += candidates.computed_distances;
            if (!candidates) {
                atomic_error = candidates.error.release();
                break;
            }

            auto match = candidates.back();
            auto woman = match.member;
            while (women_locks.atomic_set(woman.slot))
                ;

            compressed_slot_t husband_slot = woman_to_man_slots[woman.slot];
            bool woman_is_free = husband_slot == missing_slot;
            if (woman_is_free) {
                // Engagement
                man_to_woman_slots[free_man_slot] = static_cast<compressed_slot_t>(woman.slot);
                woman_to_man_slots[woman.slot] = free_man_slot;
                engagements++;
            } else {
                distance_t distance_from_husband =
                    women_metric(women_values[static_cast<compressed_slot_t>(woman.slot)], men_values[husband_slot]);
                distance_t distance_from_candidate = match.distance;
                if (distance_from_husband > distance_from_candidate) {
                    // Break-up
                    while (men_locks.atomic_set(husband_slot))
                        ;
                    man_to_woman_slots[husband_slot] = missing_slot;
                    men_locks.atomic_reset(husband_slot);

                    // New Engagement
                    man_to_woman_slots[free_man_slot] = static_cast<compressed_slot_t>(woman.slot);
                    woman_to_man_slots[woman.slot] = free_man_slot;
                    engagements++;

                    std::unique_lock<std::mutex> push_lock(free_men_mutex);
                    free_men.push(husband_slot);
                } else {
                    std::unique_lock<std::mutex> push_lock(free_men_mutex);
                    free_men.push(free_man_slot);
                }
            }

            men_locks.atomic_reset(free_man_slot);
            women_locks.atomic_reset(woman.slot);
        }
    });

    if (atomic_error)
        return result.failed(atomic_error.load());

    // Export the "slots" into keys:
    std::size_t intersection_size = 0;
    for (std::size_t man_slot = 0; man_slot != men.size(); ++man_slot) {
        compressed_slot_t woman_slot = man_to_woman_slots[man_slot];
        if (woman_slot != missing_slot) {
            man_key_t man = men.at(static_cast<compressed_slot_t>(man_slot)).key;
            woman_key_t woman = women.at(woman_slot).key;
            man_to_woman[man] = woman;
            woman_to_man[woman] = man;
            intersection_size++;
        }
    }

    // Export stats
    result.engagements = engagements;
    result.intersection_size = intersection_size;
    result.computed_distances = computed_distances;
    result.visited_members = visited_members;
    return result;
}

} // namespace usearch
} // namespace unum
