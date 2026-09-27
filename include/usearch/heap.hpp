/**
 *  @file       heap.hpp
 *  @brief      堆、uint40、哈希集、环形队列等图遍历辅助容器。
 */
#pragma once
#include <usearch/sync.hpp>
namespace unum {
namespace usearch {

/**
 *  @brief  Similar to `std::priority_queue`, but allows raw access to underlying
 *          memory, in case you want to shuffle it or sort. Good for collections
 *          from 100s to 10'000s elements.
 *
 *  In a max-heap, the heap property ensures that the value of each node is greater
 *  than or equal to the values of its children. This means that the largest element
 *  is always at the root of the heap.
 *
 *  @section    Heap Structures
 *
 *  There are several designs of heaps. Binary heaps are the simplest & most common
 *  variant, that is easy to implement as a succint array. However, they are not the
 *  most efficient for all operations. Most importantly, @b melding (merging) of
 *  two heaps has linear complexity in time.
 *
 *  +-----------------+---------+-----------+---------+--------------+---------+
 *  | Operation       | find-max| delete-max| insert  | increase-key | meld    |
 *  +-----------------+---------+-----------+---------+--------------+---------+
 *  | Binary          | Θ(1)    | Θ(log n)  | O(log n)| O(log n)     | Θ(n)    |
 *  | Leftist         | Θ(1)    | Θ(log n)  | O(log n)| Θ(log n)     | Θ(log n)|
 *  | Binomial        | Θ(1)    | Θ(log n)  | Θ(1)    | Θ(log n)     | O(log n)|
 *  | Skew binomial   | Θ(1)    | Θ(log n)  | Θ(1)    | O(log n)     | O(log n)|
 *  | Pairing         | Θ(1)    | O(log n)  | Θ(1)    | o(log n)     | Θ(1)    |
 *  | Rank-pairing    | Θ(1)    | O(log n)  | Θ(1)    | Θ(1)         | Θ(1)    |
 *  | Fibonacci       | Θ(1)    | O(log n)  | Θ(1)    | Θ(1)         | Θ(1)    |
 *  | Strict Fibonacci| Θ(1)    | O(log n)  | Θ(1)    | Θ(1)         | Θ(1)    |
 *  | Brodal          | Θ(1)    | Θ(log n)  | Θ(1)    | Θ(1)         | Θ(1)    |
 *  | 2–3 heap        | Θ(1)    | O(log n)  | Θ(1)    | Θ(1)         | O(log n)|
 *  +-----------------+---------+-----------+---------+--------------+---------+
 *
 *  It's well known, that improved priority queue structures translate into better
 *  graph-transversal algorithms. For example, Dijkstra's algorithm can be sped up
 *  by using a Fibonacci heap for arbitrary weights. For integer weight bounded
 *  by L, Schrijver reported following time complexities in 2004:
 *
 *  +------------+-------------------------------------+----------------------------+--------------------------+
 *  | Weights    | Algorithm                           | Time complexity            | Author                   |
 *  +------------+-------------------------------------+----------------------------+--------------------------+
 *  | R          |                                     | O(V^2 EL)                  | Ford 1956                |
 *  | R          | Bellman–Ford algorithm              | O(VE)                      | Shimbel 1955, Bellman    |
 *  |            |                                     |                            | 1958, Moore 1959         |
 *  | R          |                                     | O(V^2 log V)               | Dantzig 1960             |
 *  | R          | Dijkstra's with list                | O(V^2)                     | Leyzorek et al. 1957,    |
 *  |            |                                     |                            | Dijkstra 1959...         |
 *  | R          | Dijkstra's with binary heap         | O((E + V) log V)           | Johnson 1977             |
 *  | R          | Dijkstra's with Fibonacci heap      | O(E + V log V)             | Fredman & Tarjan 1984,   |
 *  |            |                                     |                            | Fredman & Tarjan 1987    |
 *  | R          | Quantum Dijkstra                    | O(√VE log^2 V)             | Dürr et al. 2006         |
 *  | R          | Dial's algorithm (Dijkstra's using  | O(E + LV)                  | Dial 1969                |
 *  |            | a bucket queue with L buckets)      |                            |                          |
 *  | N          |                                     | O(E log log L)             | Johnson 1981, Karlsson & |
 *  |            |                                     |                            | Poblete 1983             |
 *  | N          | Gabow's algorithm                   | O(E log_E/V L)             | Gabow 1983, Gabow 1985   |
 *  | N          |                                     | O(E + V √log L)            | Ahuja et al. 1990        |
 *  | N          | Thorup                              | O(E + V log log V)         | Thorup 2004              |
 *  +------------+-------------------------------------+----------------------------+--------------------------+
 *
 *  Possible improvements:
 *  - Randomized meldable heaps: https://en.wikipedia.org/wiki/Randomized_meldable_heap
 *  - D-ary heaps: https://en.wikipedia.org/wiki/D-ary_heap
 *  - B-heap: https://en.wikipedia.org/wiki/B-heap
 */
template <typename element_at,                                //
          typename comparator_at = std::less<void>,           // <void> is needed before C++14.
          typename allocator_at = std::allocator<element_at>> //
class max_heap_gt {
  public:
    using element_t = element_at;
    using comparator_t = comparator_at;
    using allocator_t = allocator_at;

    using value_type = element_t;

    static_assert(std::is_trivially_destructible<element_t>(), "This heap is designed for trivial structs");
    static_assert(std::is_trivially_copy_constructible<element_t>(), "This heap is designed for trivial structs");

  private:
    element_t* elements_;
    std::size_t size_;
    std::size_t capacity_;

  public:
    max_heap_gt() noexcept : elements_(nullptr), size_(0), capacity_(0) {}

    max_heap_gt(max_heap_gt&& other) noexcept
        : elements_(exchange(other.elements_, nullptr)), size_(exchange(other.size_, 0)),
          capacity_(exchange(other.capacity_, 0)) {}

    max_heap_gt& operator=(max_heap_gt&& other) noexcept {
        std::swap(elements_, other.elements_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }

    max_heap_gt(max_heap_gt const&) = delete;
    max_heap_gt& operator=(max_heap_gt const&) = delete;

    ~max_heap_gt() noexcept { reset(); }

    void reset() noexcept {
        if (elements_)
            allocator_t{}.deallocate(elements_, capacity_);
        elements_ = nullptr;
        capacity_ = 0;
        size_ = 0;
    }

    inline bool empty() const noexcept { return !size_; }
    inline std::size_t size() const noexcept { return size_; }
    inline std::size_t capacity() const noexcept { return capacity_; }
    inline element_t* data() noexcept { return elements_; }
    inline element_t const* data() const noexcept { return elements_; }
    inline void clear() noexcept { size_ = 0; }
    inline void shrink(std::size_t n) noexcept { size_ = (std::min<std::size_t>)(n, size_); }

    /// @brief  Selects the largest element in the heap.
    /// @return Reference to the stored element.
    inline element_t const& top() const noexcept { return elements_[0]; }

    /// @brief Invalidates the "max-heap" property, transforming into ascending range.
    inline void sort_ascending() noexcept { std::sort_heap(elements_, elements_ + size_, &less); }

    /**
     *  @brief Ensures the heap has enough capacity for the specified number of elements.
     *  @param new_capacity The desired minimum capacity.
     *  @return True if the capacity was successfully increased, false otherwise.
     */
    usearch_profiled_m bool reserve(std::size_t new_capacity) noexcept {
        usearch_profile_name_m(max_heap_reserve);
        if (new_capacity <= capacity_)
            return true;

        checked_size_result_t rounded_capacity = checked_ceil2(new_capacity);
        if (!rounded_capacity)
            return false;
        checked_size_result_t doubled_capacity = checked_mul(capacity_, std::size_t{2});
        if (!doubled_capacity)
            return false;
        new_capacity =
            (std::max<std::size_t>)(rounded_capacity.value, (std::max<std::size_t>)(doubled_capacity.value, 16u));
        auto allocator = allocator_t{};
        auto new_elements = allocator.allocate(new_capacity);
        if (!new_elements)
            return false;

        if (elements_) {
            std::memcpy(new_elements, elements_, size_ * sizeof(element_t));
            allocator.deallocate(elements_, capacity_);
        }
        elements_ = new_elements;
        capacity_ = new_capacity;
        return new_elements;
    }

    /**
     *  @brief Inserts an element into the heap.
     *  @param element The element to be inserted.
     *  @return True if the element was successfully inserted, false otherwise.
     */
    bool insert(element_t&& element) noexcept {
        if (!reserve(size_ + 1))
            return false;

        insert_reserved(std::move(element));
        return true;
    }

    /**
     *  @brief Inserts an element into the heap without reserving additional space.
     *  @param element The element to be inserted.
     */
    usearch_profiled_m void insert_reserved(element_t&& element) noexcept {
        usearch_profile_name_m(max_heap_insert_reserved);
        new (&elements_[size_]) element_t(element);
        size_++;
        shift_up(size_ - 1);
    }

    /**
     *  @brief Inserts multiple elements into the heap.
     *  @param elements Pointer to the elements to be inserted.
     *  @return True if the elements were successfully inserted, false otherwise.
     */
    inline bool insert_many(element_t const* elements) noexcept {
        // Wikipedia describes a procedure, due to Floyd, which constructs a heap from an array in linear time.
        // It also mentions a procedure for merging two heaps, of sizes 𝑛 and 𝑘, in time 𝑂(𝑘+log𝑘log𝑛).
        // Altogether, we can add 𝑘 elements to a heap of length 𝑛 in time 𝑂(𝑘+log𝑘log𝑛): first build a heap containing
        // 𝑘 elements to be inserted (takes 𝑂(𝑘) time), then merge that with the heap of size 𝑛 (takes 𝑂(𝑘+log𝑘log𝑛)
        // time). Compare this to repeated insertion, which would run in time 𝑂(𝑘log𝑛).
        return false;
    }

    usearch_profiled_m element_t pop() noexcept {
        usearch_profile_name_m(max_heap_pop);
        element_t result = top();
        std::swap(elements_[0], elements_[size_ - 1]);
        size_--;
        elements_[size_].~element_t();
        shift_down(0);
        return result;
    }

  private:
    static std::size_t parent_idx(std::size_t i) noexcept { return (i - 1u) / 2u; }
    static std::size_t left_child_idx(std::size_t i) noexcept { return (i * 2u) + 1u; }
    static std::size_t right_child_idx(std::size_t i) noexcept { return (i * 2u) + 2u; }
    static bool less(element_t const& a, element_t const& b) noexcept { return comparator_t{}(a, b); }

    /**
     *  @brief Shifts an element up to maintain the heap property.
     *         This operation is called when a new element is @b added at the end of the heap.
     *         The element is moved up until the heap property is restored.
     *  @param i Index of the element to be shifted up.
     */
    void shift_up(std::size_t i) noexcept {
        for (; i && less(elements_[parent_idx(i)], elements_[i]); i = parent_idx(i))
            std::swap(elements_[parent_idx(i)], elements_[i]);
    }

    /**
     *  @brief Shifts an element down to maintain the heap property.
     *         This operation is called when the root element is @b removed and the last element is moved to the root.
     *         The element is moved down until the heap property is restored.
     *  @param i Index of the element to be shifted down.
     */
    void shift_down(std::size_t i) noexcept {
        std::size_t max_idx = i;

        std::size_t left = left_child_idx(i);
        if (left < size_ && less(elements_[max_idx], elements_[left]))
            max_idx = left;

        std::size_t right = right_child_idx(i);
        if (right < size_ && less(elements_[max_idx], elements_[right]))
            max_idx = right;

        if (i != max_idx) {
            std::swap(elements_[i], elements_[max_idx]);
            shift_down(max_idx);
        }
    }
};

/**
 *  @brief  Similar to `std::priority_queue`, but allows raw access to underlying
 *          memory and always keeps the data sorted. Ideal for small collections
 *          under 128 elements.
 */
template <typename element_at,                                //
          typename comparator_at = std::less<void>,           // <void> is needed before C++14.
          typename allocator_at = std::allocator<element_at>> //
class sorted_buffer_gt {
  public:
    using element_t = element_at;
    using comparator_t = comparator_at;
    using allocator_t = allocator_at;

    static_assert(std::is_trivially_destructible<element_t>(), "This heap is designed for trivial structs");
    static_assert(std::is_trivially_copy_constructible<element_t>(), "This heap is designed for trivial structs");

    using value_type = element_t;

  private:
    element_t* elements_;
    std::size_t size_;
    std::size_t capacity_;

  public:
    sorted_buffer_gt() noexcept : elements_(nullptr), size_(0), capacity_(0) {}

    sorted_buffer_gt(sorted_buffer_gt&& other) noexcept
        : elements_(exchange(other.elements_, nullptr)), size_(exchange(other.size_, 0)),
          capacity_(exchange(other.capacity_, 0)) {}

    sorted_buffer_gt& operator=(sorted_buffer_gt&& other) noexcept {
        std::swap(elements_, other.elements_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }

    sorted_buffer_gt(sorted_buffer_gt const&) = delete;
    sorted_buffer_gt& operator=(sorted_buffer_gt const&) = delete;

    ~sorted_buffer_gt() noexcept { reset(); }

    void reset() noexcept {
        if (elements_)
            allocator_t{}.deallocate(elements_, capacity_);
        elements_ = nullptr;
        capacity_ = 0;
        size_ = 0;
    }

    inline bool empty() const noexcept { return !size_; }
    inline std::size_t size() const noexcept { return size_; }
    inline std::size_t capacity() const noexcept { return capacity_; }
    inline element_t const& top() const noexcept { return elements_[size_ - 1]; }
    inline void clear() noexcept { size_ = 0; }

    bool reserve(std::size_t new_capacity) noexcept {
        if (new_capacity <= capacity_)
            return true;

        checked_size_result_t rounded_capacity = checked_ceil2(new_capacity);
        if (!rounded_capacity)
            return false;
        checked_size_result_t doubled_capacity = checked_mul(capacity_, std::size_t{2});
        if (!doubled_capacity)
            return false;
        new_capacity =
            (std::max<std::size_t>)(rounded_capacity.value, (std::max<std::size_t>)(doubled_capacity.value, 16u));
        auto allocator = allocator_t{};
        auto new_elements = allocator.allocate(new_capacity);
        if (!new_elements)
            return false;

        if (size_)
            std::memcpy(new_elements, elements_, size_ * sizeof(element_t));
        if (elements_)
            allocator.deallocate(elements_, capacity_);

        elements_ = new_elements;
        capacity_ = new_capacity;
        return true;
    }

    inline void insert_reserved(element_t&& element) noexcept {
        std::size_t slot = size_ ? std::lower_bound(elements_, elements_ + size_, element, &less) - elements_ : 0;
        std::size_t to_move = size_ - slot;
        element_t* source = elements_ + size_ - 1;
        for (; to_move; --to_move, --source)
            source[1] = source[0];
        elements_[slot] = element;
        size_++;
    }

    /**
     *  @return `true` if the entry was added, `false` if it wasn't relevant enough.
     */
    inline bool insert(element_t&& element, std::size_t limit) noexcept {
        std::size_t slot = size_ ? std::lower_bound(elements_, elements_ + size_, element, &less) - elements_ : 0;
        if (slot == limit)
            return false;
        std::size_t to_move = size_ - slot - (size_ == limit);
        element_t* source = elements_ + size_ - 1 - (size_ == limit);
        for (; to_move; --to_move, --source)
            source[1] = source[0];
        elements_[slot] = element;
        size_ += size_ != limit;
        return true;
    }

    inline element_t pop() noexcept {
        size_--;
        element_t result = elements_[size_];
        elements_[size_].~element_t();
        return result;
    }

    void sort_ascending() noexcept {}
    inline void shrink(std::size_t n) noexcept { size_ = (std::min<std::size_t>)(n, size_); }

    inline element_t* data() noexcept { return elements_; }
    inline element_t const* data() const noexcept { return elements_; }

  private:
    static bool less(element_t const& a, element_t const& b) noexcept { return comparator_t{}(a, b); }
};

#if defined(USEARCH_DEFINED_WINDOWS)
#pragma pack(push, 1) // Pack struct elements on 1-byte alignment
#endif

/**
 *  @brief  Five-byte integer type to address node clouds with over 4B entries.
 *
 *  40 bits is enough to address a @b Trillion entries potentially colocated on 1 machine.
 *  At roughly 5 bytes * 20 neighbors + 100 bytes per entry, this translates to 200 TB of data,
 *  which is similar to a single-server capacity of modern NVME arrays.
 */
class usearch_pack_m uint40_t {
    unsigned char octets[5];

    inline uint40_t& broadcast(unsigned char c) {
        std::memset(octets, c, 5);
        return *this;
    }

  public:
    inline uint40_t() noexcept { broadcast(0); }
    inline uint40_t(std::uint32_t n) noexcept {
        std::memcpy(&octets, &n, 4);
        octets[4] = 0;
    }

#ifdef USEARCH_64BIT_ENV
    inline uint40_t(std::uint64_t n) noexcept { std::memcpy(octets, &n, 5); }
#endif

    uint40_t(uint40_t&&) = default;
    uint40_t(uint40_t const&) = default;
    uint40_t& operator=(uint40_t&&) = default;
    uint40_t& operator=(uint40_t const&) = default;

#if defined(USEARCH_DEFINED_CLANG) && defined(USEARCH_DEFINED_APPLE)
    inline uint40_t(std::size_t n) noexcept {
#ifdef USEARCH_64BIT_ENV
        std::memcpy(octets, &n, 5);
#else
        std::memcpy(octets, &n, 4);
        octets[4] = 0;
#endif // USEARCH_64BIT_ENV
    }
#endif // USEARCH_DEFINED_CLANG && USEARCH_DEFINED_APPLE

    inline operator std::size_t() const noexcept {
        std::size_t result = 0;
#ifdef USEARCH_64BIT_ENV
        std::memcpy(&result, octets, 5);
#else
        std::memcpy(&result, octets, 4);
#endif
        return result;
    }

    /* Parenthesized declarator keeps MSVC's preprocessor from expanding
     * `max` / `min` against `<windows.h>`'s `max(a,b)` / `min(a,b)` macros. */
    inline static uint40_t(max)() noexcept { return uint40_t{}.broadcast(0xFF); }
    inline static uint40_t(min)() noexcept { return uint40_t{}.broadcast(0); }

    inline bool operator==(uint40_t const& other) const noexcept { return std::memcmp(octets, other.octets, 5) == 0; }
    inline bool operator!=(uint40_t const& other) const noexcept { return !(*this == other); }
    inline bool operator>(uint40_t const& other) const noexcept { return other < *this; }
    inline bool operator<=(uint40_t const& other) const noexcept { return !(*this > other); }
    inline bool operator>=(uint40_t const& other) const noexcept { return !(*this < other); }
    inline bool operator<(uint40_t const& other) const noexcept {
        for (int i = 0; i < 5; ++i) {
            if (octets[4 - i] < other.octets[4 - i])
                return true;
            if (octets[4 - i] > other.octets[4 - i])
                return false;
        }
        return false;
    }
};

#if defined(USEARCH_DEFINED_WINDOWS)
#pragma pack(pop) // Reset alignment to default
#endif

static_assert(sizeof(uint40_t) == 5, "uint40_t must be exactly 5 bytes");

/**
 *  @brief  Reflection-helper to get the default "unused" value for a given type.
 *          Needed to initialize hash-sets and bit-sets.
 */
template <typename element_at> struct default_free_value_gt {
    template <typename sfinae_element_at = element_at,
              typename std::enable_if<std::is_integral<sfinae_element_at>::value>::type* = nullptr>
    static sfinae_element_at value() noexcept {
        return (std::numeric_limits<element_at>::max)();
    }
    template <typename sfinae_element_at = element_at,
              typename std::enable_if<!std::is_integral<sfinae_element_at>::value>::type* = nullptr>
    static sfinae_element_at value() noexcept {
        return element_at();
    }
};

template <> struct default_free_value_gt<uint40_t> {
    static uint40_t value() noexcept { return (uint40_t::max)(); }
};

template <typename element_at> element_at default_free_value() { return default_free_value_gt<element_at>::value(); }

/**
 *  @brief  Adapter to allow definining arbitrary hash functions for keys and slots.
 *          It's added, as overloading `std::hash` is not recommended by the standard.
 */
template <typename element_at> struct hash_gt {
    std::size_t operator()(element_at const& element) const noexcept { return std::hash<element_at>{}(element); }
};

/**
 *  @brief  SplitMix64 finalizer, used to scatter integer keys before masking.
 *
 *  On libstdc++ and libc++ `std::hash` is the identity for integers. Our open-addressing
 *  tables mask that hash into a power-of-2 slot count and probe linearly until an @b empty
 *  slot, so consecutive keys land in adjacent slots and merge into one contiguous run.
 *  Both insertions and lookups then scan that run end-to-end, which is quadratic overall:
 *  a dense ascending key range collapses insertion throughput by three orders of magnitude.
 *  Mixing costs ~20ns per key and is dwarfed by the graph traversal in `add`.
 */
template <> struct hash_gt<std::uint64_t> {
    std::size_t operator()(std::uint64_t const& element) const noexcept {
        std::uint64_t x = element;
        x = (x ^ (x >> 30u)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27u)) * 0x94D049BB133111EBULL;
        return static_cast<std::size_t>(x ^ (x >> 31u));
    }
};

template <> struct hash_gt<std::int64_t> {
    std::size_t operator()(std::int64_t const& element) const noexcept {
        return hash_gt<std::uint64_t>{}(static_cast<std::uint64_t>(element));
    }
};

template <> struct hash_gt<uint40_t> {
    std::size_t operator()(uint40_t const& element) const noexcept { return std::hash<std::size_t>{}(element); }
};

/**
 *  @brief  Minimalistic hash-set implementation to track visited nodes during graph traversal.
 *          In our primary usecase, its a sparse alternative to a bit-set.
 *
 *  It doesn't support deletion of separate objects, but supports `clear`-ing all at once.
 *  It expects `reserve` to be called ahead of all insertions, so no resizes are needed.
 *  It also assumes `0xFF...FF` slots to be unused, to simplify the design.
 *  It uses linear probing, the number of slots is always a power of two, and it uses linear-probing
 *  in case of bucket collisions.
 */
template <typename element_at, typename hasher_at = hash_gt<element_at>, typename allocator_at = std::allocator<byte_t>>
class growing_hash_set_gt {

    using element_t = element_at;
    using hasher_t = hasher_at;

    using allocator_t = allocator_at;
    using byte_t = typename allocator_t::value_type;
    static_assert(sizeof(byte_t) == 1, "Allocator must allocate separate addressable bytes");

    element_t* slots_{};
    /// @brief Number of slots.
    std::size_t capacity_{};
    /// @brief Number of populated.
    std::size_t count_{};
    hasher_t hasher_{};

  public:
    growing_hash_set_gt() noexcept {}
    ~growing_hash_set_gt() noexcept { reset(); }

    explicit operator bool() const noexcept { return slots_; }
    std::size_t size() const noexcept { return count_; }

    void clear() noexcept {
        if (slots_)
            std::memset((void*)slots_, 0xFF, capacity_ * sizeof(element_t));
        count_ = 0;
    }

    void reset() noexcept {
        if (slots_)
            allocator_t{}.deallocate((byte_t*)slots_, capacity_ * sizeof(element_t));
        slots_ = nullptr;
        capacity_ = 0;
        count_ = 0;
    }

    growing_hash_set_gt(std::size_t capacity) noexcept : count_(0u) {
        checked_size_result_t slots_count = checked_ceil2(capacity);
        checked_size_result_t bytes = slots_count ? checked_mul(slots_count.value, sizeof(element_t)) : slots_count;
        slots_ = bytes ? (element_t*)allocator_t{}.allocate(bytes.value) : nullptr;
        capacity_ = slots_ ? slots_count.value : 0u;
        clear();
    }

    growing_hash_set_gt(growing_hash_set_gt&& other) noexcept {
        slots_ = exchange(other.slots_, nullptr);
        capacity_ = exchange(other.capacity_, 0);
        count_ = exchange(other.count_, 0);
    }

    growing_hash_set_gt& operator=(growing_hash_set_gt&& other) noexcept {
        std::swap(slots_, other.slots_);
        std::swap(capacity_, other.capacity_);
        std::swap(count_, other.count_);
        return *this;
    }

    growing_hash_set_gt(growing_hash_set_gt const&) = delete;
    growing_hash_set_gt& operator=(growing_hash_set_gt const&) = delete;

    /**
     *  @brief  Checks if the element is already in the hash-set.
     *  @return `true` if the element is already in the hash-set.
     */
    inline bool test(element_t const& elem) const noexcept {
        std::size_t index = hasher_(elem) & (capacity_ - 1);
        while (slots_[index] != default_free_value<element_t>()) {
            if (slots_[index] == elem)
                return true;

            index = (index + 1) & (capacity_ - 1);
        }
        return false;
    }

    /// 只读探测：供 HNSW 软件流水在测距前预取「下一个未访问」邻居，避免误用 set 污染 visits。
    inline bool contains(element_t const& elem) const noexcept {
        if (!slots_ || !capacity_)
            return false;
        std::size_t index = hasher_(elem) & (capacity_ - 1);
        while (slots_[index] != default_free_value<element_t>()) {
            if (slots_[index] == elem)
                return true;
            index = (index + 1) & (capacity_ - 1);
        }
        return false;
    }

    /**
     *  @brief  Inserts an element into the hash-set.
     *  @return Similar to `bitset_gt`, returns the previous value.
     */
    inline bool set(element_t const& elem) noexcept {
        std::size_t index = hasher_(elem) & (capacity_ - 1);
        while (slots_[index] != default_free_value<element_t>()) {
            // Already exists
            if (slots_[index] == elem)
                return true;

            index = (index + 1) & (capacity_ - 1);
        }
        slots_[index] = elem;
        ++count_;
        return false;
    }

    /**
     *  @brief  Extends the capacity of the hash-set.
     *  @return `true` if enough capacity is available, `false` if memory allocation failed.
     */
    bool reserve(std::size_t new_capacity) noexcept {
        checked_size_result_t scaled_capacity = checked_mul(new_capacity, std::size_t{5});
        if (!scaled_capacity)
            return false;
        new_capacity = scaled_capacity.value / 3u;
        if (new_capacity <= capacity_)
            return true;

        checked_size_result_t rounded_capacity = checked_ceil2(new_capacity);
        if (!rounded_capacity)
            return false;
        new_capacity = rounded_capacity.value;
        checked_size_result_t new_bytes = checked_mul(new_capacity, sizeof(element_t));
        if (!new_bytes)
            return false;
        element_t* new_slots = (element_t*)allocator_t{}.allocate(new_bytes.value);
        if (!new_slots)
            return false;

        std::memset((void*)new_slots, 0xFF, new_capacity * sizeof(element_t));
        std::size_t new_count = count_;
        if (count_) {
            for (std::size_t old_index = 0; old_index != capacity_; ++old_index) {
                if (slots_[old_index] == default_free_value<element_t>())
                    continue;

                std::size_t new_index = hasher_(slots_[old_index]) & (new_capacity - 1);
                while (new_slots[new_index] != default_free_value<element_t>())
                    new_index = (new_index + 1) & (new_capacity - 1);
                new_slots[new_index] = slots_[old_index];
            }
        }

        reset();
        slots_ = new_slots;
        capacity_ = new_capacity;
        count_ = new_count;
        return true;
    }
};

/**
 *  @brief  Basic single-threaded @b ring class, used for all kinds of task queues.
 */
template <typename element_at, typename allocator_at = std::allocator<element_at>> //
class ring_gt {
  public:
    using element_t = element_at;
    using allocator_t = allocator_at;

    static_assert(std::is_trivially_destructible<element_t>(), "This ring is designed for trivial structs");
    static_assert(std::is_trivially_copy_constructible<element_t>(), "This ring is designed for trivial structs");

    using value_type = element_t;

  private:
    element_t* elements_{};
    std::size_t capacity_{};
    std::size_t head_{};
    std::size_t tail_{};
    bool empty_{true};
    allocator_t allocator_{};

  public:
    explicit ring_gt(allocator_t const& alloc = allocator_t()) noexcept : allocator_(alloc) {}

    ring_gt(ring_gt const&) = delete;
    ring_gt& operator=(ring_gt const&) = delete;

    ring_gt(ring_gt&& other) noexcept { swap(other); }
    ring_gt& operator=(ring_gt&& other) noexcept {
        swap(other);
        return *this;
    }

    void swap(ring_gt& other) noexcept {
        std::swap(elements_, other.elements_);
        std::swap(capacity_, other.capacity_);
        std::swap(head_, other.head_);
        std::swap(tail_, other.tail_);
        std::swap(empty_, other.empty_);
        std::swap(allocator_, other.allocator_);
    }

    ~ring_gt() noexcept { reset(); }

    bool empty() const noexcept { return empty_; }
    size_t capacity() const noexcept { return capacity_; }
    size_t size() const noexcept {
        if (empty_)
            return 0;
        else if (head_ > tail_)
            return head_ - tail_;
        else
            return capacity_ - (tail_ - head_);
    }

    void clear() noexcept {
        head_ = 0;
        tail_ = 0;
        empty_ = true;
    }

    void reset() noexcept {
        if (elements_)
            allocator_.deallocate(elements_, capacity_);
        elements_ = nullptr;
        capacity_ = 0;
        head_ = 0;
        tail_ = 0;
        empty_ = true;
    }

    bool reserve(std::size_t n) noexcept {
        if (n < size())
            return false; // prevent data loss
        if (n <= capacity())
            return true;
        checked_size_result_t rounded_capacity = checked_ceil2(n);
        if (!rounded_capacity)
            return false;
        n = (std::max<std::size_t>)(rounded_capacity.value, 64u);
        element_t* elements = allocator_.allocate(n);
        if (!elements)
            return false;

        std::size_t i = 0;
        while (try_pop(elements[i]))
            i++;

        reset();
        elements_ = elements;
        capacity_ = n;
        head_ = i;
        tail_ = 0;
        empty_ = (i == 0);
        return true;
    }

    void push(element_t const& value) usearch_noexcept_m {
        usearch_assert_m(capacity() > 0, "Ring buffer is not initialized");
        usearch_assert_m(size() < capacity(), "Ring buffer is full");
        elements_[head_] = value;
        head_ = (head_ + 1) % capacity_;
        empty_ = false;
    }

    bool try_push(element_t const& value) noexcept {
        if (head_ == tail_ && !empty_)
            return false; // `elements_` is full
        push(value);
        return true;
    }

    bool try_pop(element_t& value) noexcept {
        if (empty_)
            return false;

        value = std::move(elements_[tail_]);
        tail_ = (tail_ + 1) % capacity_;
        empty_ = head_ == tail_;
        return true;
    }

    element_t const& operator[](std::size_t i) const noexcept { return elements_[(tail_ + i) % capacity_]; }
};

} // namespace usearch
} // namespace unum
