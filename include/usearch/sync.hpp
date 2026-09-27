/**
 *  @file       sync.hpp
 *  @brief      图突变用 bitset 与 cache-line 条带锁；禁止依赖 plugins/dense。
 */
#pragma once
#include <usearch/util.hpp>
namespace unum {
namespace usearch {

/**
 *  @brief  Light-weight bitset implementation to sync nodes updates during graph mutations.
 *          Extends basic functionality with @b atomic operations.
 */
template <typename allocator_at = std::allocator<byte_t>> class bitset_gt {
    using allocator_t = allocator_at;
    using byte_t = typename allocator_t::value_type;
    static_assert(sizeof(byte_t) == 1, "Allocator must allocate separate addressable bytes");

    using compressed_slot_t = unsigned long;

    static constexpr std::size_t bits_per_slot() { return sizeof(compressed_slot_t) * CHAR_BIT; }
    static constexpr compressed_slot_t bits_mask() { return sizeof(compressed_slot_t) * CHAR_BIT - 1; }
    static constexpr std::size_t bits_slots(std::size_t bits) { return divide_round_up<bits_per_slot()>(bits); }

    compressed_slot_t* slots_{};
    /// @brief Number of slots.
    std::size_t count_{};

  public:
    bitset_gt() noexcept {}
    ~bitset_gt() noexcept { reset(); }

    explicit operator bool() const noexcept { return slots_; }
    void clear() noexcept {
        if (slots_)
            std::memset(slots_, 0, count_ * sizeof(compressed_slot_t));
    }

    void reset() noexcept {
        if (slots_)
            allocator_t{}.deallocate((byte_t*)slots_, count_ * sizeof(compressed_slot_t));
        slots_ = nullptr;
        count_ = 0;
    }

    bitset_gt(std::size_t capacity) noexcept {
        checked_size_result_t slots_count = checked_divide_round_up(capacity, bits_per_slot());
        checked_size_result_t bytes =
            slots_count ? checked_mul(slots_count.value, sizeof(compressed_slot_t)) : slots_count;
        slots_ = bytes ? (compressed_slot_t*)allocator_t{}.allocate(bytes.value) : nullptr;
        count_ = slots_ ? slots_count.value : 0u;
        clear();
    }

    bitset_gt(bitset_gt&& other) noexcept {
        slots_ = exchange(other.slots_, nullptr);
        count_ = exchange(other.count_, 0);
    }

    bitset_gt& operator=(bitset_gt&& other) noexcept {
        std::swap(slots_, other.slots_);
        std::swap(count_, other.count_);
        return *this;
    }

    bitset_gt(bitset_gt const&) = delete;
    bitset_gt& operator=(bitset_gt const&) = delete;

    inline bool test(std::size_t i) const noexcept { return slots_[i / bits_per_slot()] & (1ul << (i & bits_mask())); }
    inline bool set(std::size_t i) noexcept {
        compressed_slot_t& slot = slots_[i / bits_per_slot()];
        compressed_slot_t mask{1ul << (i & bits_mask())};
        bool value = slot & mask;
        slot |= mask;
        return value;
    }

#if defined(USEARCH_DEFINED_WINDOWS)

    inline bool atomic_set(std::size_t i) noexcept {
        compressed_slot_t mask{1ul << (i & bits_mask())};
        return InterlockedOr((long volatile*)&slots_[i / bits_per_slot()], mask) & mask;
    }

    inline void atomic_reset(std::size_t i) noexcept {
        compressed_slot_t mask{1ul << (i & bits_mask())};
        InterlockedAnd((long volatile*)&slots_[i / bits_per_slot()], ~mask);
    }

#else

    inline bool atomic_set(std::size_t i) noexcept {
        compressed_slot_t mask{1ul << (i & bits_mask())};
        return __atomic_fetch_or(&slots_[i / bits_per_slot()], mask, __ATOMIC_ACQUIRE) & mask;
    }

    inline void atomic_reset(std::size_t i) noexcept {
        compressed_slot_t mask{1ul << (i & bits_mask())};
        __atomic_fetch_and(&slots_[i / bits_per_slot()], ~mask, __ATOMIC_RELEASE);
    }

#endif

    class lock_t {
        bitset_gt& bitset_;
        std::size_t bit_offset_;

      public:
        inline ~lock_t() noexcept { bitset_.atomic_reset(bit_offset_); }
        inline lock_t(bitset_gt& bitset, std::size_t bit_offset) noexcept : bitset_(bitset), bit_offset_(bit_offset) {
            while (bitset_.atomic_set(bit_offset_))
                ;
        }
    };

    inline lock_t lock(std::size_t i) noexcept { return {*this, i}; }
};

using bitset_t = bitset_gt<>;

/**
 *  @brief  Cache-line-padded striped spin-lock array for concurrent graph mutations.
 *          Maps node slots to lock stripes via Fibonacci hashing, with each stripe
 *          occupying its own cache line to eliminate false sharing.
 *          The number of stripes is proportional to `threads * connectivity`, not
 *          graph size, keeping the lock array comfortably within L2/L3 cache.
 */
template <typename allocator_at = std::allocator<byte_t>, std::size_t cache_line_ak = 128> //
class striped_locks_gt {
    using allocator_t = allocator_at;
    using byte_t = typename allocator_t::value_type;
    static_assert(sizeof(byte_t) == 1, "Allocator must allocate separate addressable bytes");

    static constexpr std::uint64_t fibonacci_k = 0x9E3779B97F4A7C15ull;

    using atomic_flag_t = std::atomic<std::uint8_t>;
    struct alignas(cache_line_ak) padded_lock_t {
        atomic_flag_t flag{0};
        char padding_[cache_line_ak - sizeof(atomic_flag_t)];
    };
    static_assert(sizeof(padded_lock_t) == cache_line_ak, "Lock stripe must be exactly one cache line");

    // `padded_lock_t` is `alignas(cache_line_ak)` (128 B by default) which
    // exceeds what a plain allocator guarantees (typically 16 B on x86-64).
    // Rather than demanding an over-aligned allocator, we over-allocate and
    // keep a pointer to the aligned sub-region — `raw_` is what we hand back
    // to the allocator, `stripes_` is the aligned view used for reads/writes.
    byte_t* raw_{};
    std::size_t raw_bytes_{};
    padded_lock_t* stripes_{};
    std::size_t count_{};
    unsigned shift_{};

    inline std::size_t stripe_for_(std::size_t slot) const noexcept {
        return static_cast<std::size_t>((static_cast<std::uint64_t>(slot) * fibonacci_k) >> shift_);
    }

  public:
    striped_locks_gt() noexcept {}
    ~striped_locks_gt() noexcept { reset(); }

    explicit operator bool() const noexcept { return stripes_; }

    void reset() noexcept {
        if (stripes_)
            for (std::size_t i = 0; i < count_; i++)
                stripes_[i].~padded_lock_t();
        if (raw_)
            allocator_t{}.deallocate(raw_, raw_bytes_);
        raw_ = nullptr;
        raw_bytes_ = 0;
        stripes_ = nullptr;
        count_ = 0;
        shift_ = 64;
    }

    striped_locks_gt(std::size_t threads, std::size_t connectivity) noexcept {
        checked_size_result_t desired = checked_mul(threads, connectivity);
        desired = desired ? checked_mul(desired.value, std::size_t{4}) : desired;
        if (!desired) {
            shift_ = 64;
            return;
        }

        checked_size_result_t count = checked_ceil2((std::max<std::size_t>)(desired.value, 256));
        if (!count) {
            shift_ = 64;
            return;
        }
        count_ = count.value;
        shift_ = 64;
        for (std::size_t n = count_; n > 1; n >>= 1)
            shift_--;
        // Request one extra stripe's worth of slack so we can always land on a
        // `cache_line_ak`-aligned address inside the allocation, regardless of
        // what the underlying allocator returns.
        constexpr std::size_t alignment_k = alignof(padded_lock_t);
        checked_size_result_t raw_bytes = checked_mul_add(count_, sizeof(padded_lock_t), alignment_k);
        if (!raw_bytes) {
            count_ = 0;
            shift_ = 64;
            return;
        }
        raw_bytes_ = raw_bytes.value;
        raw_ = allocator_t{}.allocate(raw_bytes_);
        if (!raw_) {
            raw_bytes_ = 0;
            count_ = 0;
            shift_ = 64;
            return;
        }
        auto raw_address = reinterpret_cast<std::uintptr_t>(raw_);
        auto aligned_address = (raw_address + alignment_k - 1) & ~(static_cast<std::uintptr_t>(alignment_k) - 1);
        stripes_ = reinterpret_cast<padded_lock_t*>(aligned_address);
        for (std::size_t i = 0; i < count_; i++)
            new (&stripes_[i]) padded_lock_t();
    }

    striped_locks_gt(striped_locks_gt&& other) noexcept {
        raw_ = exchange(other.raw_, (byte_t*)nullptr);
        raw_bytes_ = exchange(other.raw_bytes_, std::size_t{0});
        stripes_ = exchange(other.stripes_, nullptr);
        count_ = exchange(other.count_, std::size_t{0});
        shift_ = exchange(other.shift_, unsigned{64});
    }

    striped_locks_gt& operator=(striped_locks_gt&& other) noexcept {
        std::swap(raw_, other.raw_);
        std::swap(raw_bytes_, other.raw_bytes_);
        std::swap(stripes_, other.stripes_);
        std::swap(count_, other.count_);
        std::swap(shift_, other.shift_);
        return *this;
    }

    striped_locks_gt(striped_locks_gt const&) = delete;
    striped_locks_gt& operator=(striped_locks_gt const&) = delete;

    inline bool atomic_set(std::size_t i) noexcept {
        return stripes_[stripe_for_(i)].flag.exchange(1, std::memory_order_acquire);
    }

    inline void atomic_reset(std::size_t i) noexcept {
        stripes_[stripe_for_(i)].flag.store(0, std::memory_order_release);
    }

    inline void lock(std::size_t i) noexcept {
        while (atomic_set(i))
            std::this_thread::yield();
    }

    inline void unlock(std::size_t i) noexcept { atomic_reset(i); }
};

} // namespace usearch
} // namespace unum
