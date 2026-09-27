/**
 *  @file       alloc.hpp
 *  @brief      对齐/页/mmap 分配器与 unfair_shared_mutex。
 */
#pragma once
#include <plugins/exec.hpp>
namespace unum {
namespace usearch {

/**
 *  @brief  Uses OS-specific APIs for aligned memory allocations.
 *          Available since C11, but only C++17, so we wrap the C version.
 */
template <typename element_at = char, std::size_t alignment_ak = 64> //
class aligned_allocator_gt {
  public:
    using value_type = element_at;
    using size_type = std::size_t;
    using pointer = element_at*;
    using const_pointer = element_at const*;
    template <typename other_element_at> struct rebind {
        using other = aligned_allocator_gt<other_element_at>;
    };

    constexpr std::size_t alignment() const { return alignment_ak; }

    pointer allocate(size_type length) const {
        checked_size_result_t bytes = checked_mul(length, sizeof(value_type));
        if (!bytes)
            return nullptr;
        checked_size_result_t length_bytes = checked_round_up(bytes.value, alignment_ak);
        if (!length_bytes)
            return nullptr;

        std::size_t alignment = alignment_ak;
#if defined(USEARCH_DEFINED_WINDOWS)
        return (pointer)_aligned_malloc(length_bytes.value, alignment);
#elif defined(USEARCH_DEFINED_APPLE) || defined(USEARCH_DEFINED_ANDROID)
        // Apple Clang keeps complaining that `aligned_alloc` is only available
        // with macOS 10.15 and newer or Android API >= 28, so let's use `posix_memalign` there.
        void* result = nullptr;
        int status = posix_memalign(&result, alignment, length_bytes.value);
        return status == 0 ? (pointer)result : nullptr;
#else
        return (pointer)aligned_alloc(alignment, length_bytes.value);
#endif
    }

    void deallocate(pointer begin, size_type) const {
#if defined(USEARCH_DEFINED_WINDOWS)
        _aligned_free(begin);
#else
        free(begin);
#endif
    }
};

using aligned_allocator_t = aligned_allocator_gt<>;

/**
 *  @brief  A simple RAM-page allocator that uses the OS-specific APIs for memory allocation.
 *          Shouldn't be used frequently, as system calls are slow.
 */
class page_allocator_t {
  public:
    static constexpr std::size_t page_size() { return 4096; }

    /**
     *  @brief Allocates an @b uninitialized block of memory of the specified size.
     *  @param count_bytes The number of bytes to allocate.
     *  @return A pointer to the allocated memory block, or `nullptr` if allocation fails.
     */
    byte_t* allocate(std::size_t count_bytes) const noexcept {
        checked_size_result_t rounded_bytes = checked_round_up(count_bytes, page_size());
        if (!rounded_bytes)
            return nullptr;
        count_bytes = rounded_bytes.value;
#if defined(USEARCH_DEFINED_WINDOWS)
        return (byte_t*)(::VirtualAlloc(NULL, count_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
        auto* result = mmap(NULL, count_bytes, PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return (result == MAP_FAILED) ? nullptr : (byte_t*)result;
#endif
    }

    void deallocate(byte_t* page_pointer, std::size_t count_bytes) const noexcept {
#if defined(USEARCH_DEFINED_WINDOWS)
        ::VirtualFree(page_pointer, 0, MEM_RELEASE);
#else
        checked_size_result_t rounded_bytes = checked_round_up(count_bytes, page_size());
        if (!rounded_bytes)
            return;
        count_bytes = rounded_bytes.value;
        munmap(page_pointer, count_bytes);
#endif
    }
};

/**
 *  @brief  Memory-mapping allocator designed for "alloc many, free at once" usage patterns.
 *          @b Thread-safe, @b except constructors and destructors.
 *
 *  Using this memory allocator won't affect your overall speed much, as that is not the bottleneck.
 *  However, it can drastically improve memory usage especially for huge indexes of small vectors.
 */
template <std::size_t alignment_ak = 1> class memory_mapping_allocator_gt {

    static constexpr std::size_t min_capacity() { return 1024 * 1024 * 4; }
    static constexpr std::size_t capacity_multiplier() { return 2; }
    static constexpr std::size_t head_size() {
        /// Pointer to the the previous arena and the size of the current one.
        return divide_round_up<alignment_ak>(sizeof(byte_t*) + sizeof(std::size_t)) * alignment_ak;
    }

    std::mutex mutex_;
    byte_t* last_arena_ = nullptr;
    std::size_t last_usage_ = head_size();
    std::size_t last_capacity_ = min_capacity();
    std::size_t wasted_space_ = 0;
    std::size_t total_allocated_ = 0;

  public:
    using value_type = byte_t;
    using size_type = std::size_t;
    using pointer = byte_t*;
    using const_pointer = byte_t const*;

    memory_mapping_allocator_gt() = default;
    memory_mapping_allocator_gt(memory_mapping_allocator_gt&& other) noexcept
        : last_arena_(exchange(other.last_arena_, nullptr)), last_usage_(exchange(other.last_usage_, 0)),
          last_capacity_(exchange(other.last_capacity_, 0)), wasted_space_(exchange(other.wasted_space_, 0)),
          total_allocated_(exchange(other.total_allocated_, 0)) {}

    memory_mapping_allocator_gt& operator=(memory_mapping_allocator_gt&& other) noexcept {
        std::swap(last_arena_, other.last_arena_);
        std::swap(last_usage_, other.last_usage_);
        std::swap(last_capacity_, other.last_capacity_);
        std::swap(wasted_space_, other.wasted_space_);
        std::swap(total_allocated_, other.total_allocated_);
        return *this;
    }

    ~memory_mapping_allocator_gt() noexcept { reset(); }

    /**
     *  @brief Discards all previously allocated memory buffers.
     */
    void reset() noexcept {
        byte_t* last_arena = last_arena_;
        while (last_arena) {
            byte_t* previous_arena = nullptr;
            std::memcpy(&previous_arena, last_arena, sizeof(byte_t*));
            std::size_t last_cap = 0;
            std::memcpy(&last_cap, last_arena + sizeof(byte_t*), sizeof(std::size_t));
            page_allocator_t{}.deallocate(last_arena, last_cap);
            last_arena = previous_arena;
        }

        // Clear the references:
        last_arena_ = nullptr;
        last_usage_ = head_size();
        last_capacity_ = min_capacity();
        wasted_space_ = 0;
        total_allocated_ = 0;
    }

    /**
     *  @brief Copy constructor.
     *  @note This is a no-op copy constructor since the allocator is not copyable.
     */
    memory_mapping_allocator_gt(memory_mapping_allocator_gt const&) noexcept {}

    /**
     *  @brief Copy assignment operator.
     *  @note This is a no-op copy assignment operator since the allocator is not copyable.
     *  @return Reference to the allocator after the assignment.
     */
    memory_mapping_allocator_gt& operator=(memory_mapping_allocator_gt const&) noexcept {
        reset();
        return *this;
    }

    /**
     *  @brief Allocates an @b uninitialized block of memory of the specified size.
     *  @param count_bytes The number of bytes to allocate.
     *  @return A pointer to the allocated memory block, or `nullptr` if allocation fails.
     */
    inline byte_t* allocate(std::size_t count_bytes) noexcept {
        checked_size_result_t extended_bytes = checked_round_up(count_bytes, alignment_ak);
        if (!extended_bytes)
            return nullptr;
        std::unique_lock<std::mutex> lock(mutex_);
        checked_size_result_t next_usage = checked_add(last_usage_, extended_bytes.value);
        if (!next_usage)
            return nullptr;
        if (!last_arena_ || (next_usage.value >= last_capacity_)) {
            checked_size_result_t rounded_bytes = checked_ceil2(extended_bytes.value);
            if (!rounded_bytes)
                return nullptr;
            checked_size_result_t new_cap =
                checked_mul((std::max)(last_capacity_, rounded_bytes.value), capacity_multiplier());
            if (!new_cap)
                return nullptr;
            checked_size_result_t new_total_allocated = checked_add(total_allocated_, new_cap.value);
            if (!new_total_allocated)
                return nullptr;
            byte_t* new_arena = page_allocator_t{}.allocate(new_cap.value);
            if (!new_arena)
                return nullptr;
            std::memcpy(new_arena, &last_arena_, sizeof(byte_t*));
            std::memcpy(new_arena + sizeof(byte_t*), &new_cap.value, sizeof(std::size_t));

            wasted_space_ += total_reserved();
            last_arena_ = new_arena;
            last_capacity_ = new_cap.value;
            last_usage_ = head_size();
            total_allocated_ = new_total_allocated.value;
        }

        wasted_space_ += extended_bytes.value - count_bytes;
        return last_arena_ + exchange(last_usage_, last_usage_ + extended_bytes.value);
    }

    /**
     *  @brief Returns the amount of memory used by the allocator across all arenas.
     *  @return The amount of space in bytes.
     */
    std::size_t total_allocated() const noexcept { return total_allocated_; }

    /**
     *  @brief Returns the amount of wasted space due to alignment.
     *  @return The amount of wasted space in bytes.
     */
    std::size_t total_wasted() const noexcept { return wasted_space_; }

    /**
     *  @brief Returns the amount of remaining memory already reserved but not yet used.
     *  @return The amount of reserved memory in bytes.
     */
    std::size_t total_reserved() const noexcept { return last_arena_ ? last_capacity_ - last_usage_ : 0; }

    /**
     *  @warning The very first memory de-allocation discards all the arenas!
     */
    void deallocate(byte_t* = nullptr, std::size_t = 0) noexcept { reset(); }
};

using memory_mapping_allocator_t = memory_mapping_allocator_gt<>;

/**
 *  @brief  C++11 userspace implementation of an oversimplified `std::shared_mutex`,
 *          that assumes rare interleaving of shared and unique locks. It's not fair,
 *          but requires only a single 32-bit atomic integer to work.
 */
class unfair_shared_mutex_t {
    /** Any positive integer describes the number of concurrent readers */
    enum state_t : std::int32_t {
        idle_k = 0,
        writing_k = -1,
    };
    std::atomic<std::int32_t> state_{idle_k};

  public:
    inline void lock() noexcept {
        std::int32_t raw;
    relock:
        raw = idle_k;
        if (!state_.compare_exchange_weak(raw, writing_k, std::memory_order_acquire, std::memory_order_relaxed)) {
            std::this_thread::yield();
            goto relock;
        }
    }

    inline void unlock() noexcept { state_.store(idle_k, std::memory_order_release); }

    inline void lock_shared() noexcept {
        std::int32_t raw;
    relock_shared:
        raw = state_.load(std::memory_order_acquire);
        // Spin while it's uniquely locked
        if (raw == writing_k) {
            std::this_thread::yield();
            goto relock_shared;
        }
        // Try incrementing the counter
        if (!state_.compare_exchange_weak(raw, raw + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            std::this_thread::yield();
            goto relock_shared;
        }
    }

    inline void unlock_shared() noexcept { state_.fetch_sub(1, std::memory_order_release); }

    /**
     *  @brief Try upgrades the current `lock_shared()` to a unique `lock()` state.
     */
    inline bool try_escalate() noexcept {
        std::int32_t one_read = 1;
        return state_.compare_exchange_weak(one_read, writing_k, std::memory_order_acquire, std::memory_order_relaxed);
    }

    /**
     *  @brief Escalates current lock potentially loosing control in the middle.
     *  It's a shortcut for `try_escalate`-`unlock_shared`-`lock` trio.
     */
    inline void unsafe_escalate() noexcept {
        if (!try_escalate()) {
            unlock_shared();
            lock();
        }
    }

    /**
     *  @brief Upgrades the current `lock_shared()` to a unique `lock()` state.
     */
    inline void escalate() noexcept {
        while (!try_escalate())
            std::this_thread::yield();
    }

    /**
     *  @brief De-escalation of a previously escalated state.
     */
    inline void de_escalate() noexcept {
        std::int32_t one_read = 1;
        state_.store(one_read, std::memory_order_release);
    }
};

template <typename mutex_at = unfair_shared_mutex_t> class shared_lock_gt {
    mutex_at& mutex_;

  public:
    inline explicit shared_lock_gt(mutex_at& m) noexcept : mutex_(m) { mutex_.lock_shared(); }
    inline ~shared_lock_gt() noexcept { mutex_.unlock_shared(); }
};

} // namespace usearch
} // namespace unum
