/**
 *  @file       util.hpp
 *  @brief      算术安全、错位访存、span/buffer、error_t、expected_gt。
 */
#pragma once
#include <usearch/setup.hpp>
namespace unum {
namespace usearch {

using byte_t = char;

struct checked_size_result_t {
    std::size_t value;
    bool overflow;
    constexpr checked_size_result_t(std::size_t value = 0, bool overflow = false) noexcept
        : value(value), overflow(overflow) {}
    constexpr explicit operator bool() const noexcept { return !overflow; }
};

constexpr checked_size_result_t checked_size_overflow() noexcept { return {0, true}; }

constexpr checked_size_result_t checked_size_from_u64(std::uint64_t value) noexcept {
    return value > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())
               ? checked_size_overflow()
               : checked_size_result_t{static_cast<std::size_t>(value), false};
}

constexpr checked_size_result_t checked_add(std::size_t a, std::size_t b) noexcept {
    return (std::numeric_limits<std::size_t>::max)() - a < b ? checked_size_overflow()
                                                             : checked_size_result_t{a + b, false};
}

constexpr checked_size_result_t checked_mul(std::size_t a, std::size_t b) noexcept {
    return a && b > (std::numeric_limits<std::size_t>::max)() / a ? checked_size_overflow()
                                                                  : checked_size_result_t{a * b, false};
}

constexpr checked_size_result_t checked_mul_add_(checked_size_result_t product, std::size_t c) noexcept {
    return product ? checked_add(product.value, c) : product;
}

constexpr checked_size_result_t checked_mul_add(std::size_t a, std::size_t b, std::size_t c) noexcept {
    return checked_mul_add_(checked_mul(a, b), c);
}

template <std::size_t multiple_ak> std::size_t divide_round_up(std::size_t num) noexcept {
    return (num + multiple_ak - 1) / multiple_ak;
}

inline std::size_t divide_round_up(std::size_t num, std::size_t denominator) noexcept {
    return (num + denominator - 1) / denominator;
}

constexpr checked_size_result_t checked_divide_round_up(std::size_t num, std::size_t denominator) noexcept {
    return !denominator ? checked_size_overflow()
           : (std::numeric_limits<std::size_t>::max)() - num < denominator - 1
               ? checked_size_overflow()
               : checked_size_result_t{(num + denominator - 1) / denominator, false};
}

constexpr checked_size_result_t checked_round_up_(checked_size_result_t quotient, std::size_t multiple) noexcept {
    return quotient ? checked_mul(quotient.value, multiple) : quotient;
}

constexpr checked_size_result_t checked_round_up(std::size_t num, std::size_t multiple) noexcept {
    return checked_round_up_(checked_divide_round_up(num, multiple), multiple);
}

inline std::size_t ceil2(std::size_t v) noexcept {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
#ifdef USEARCH_64BIT_ENV
    v |= v >> 32;
#endif
    v++;
    return v;
}

inline checked_size_result_t checked_ceil2(std::size_t v) noexcept {
    if (!v)
        return checked_size_result_t{0, false};
    if (v > (std::size_t{1} << ((sizeof(std::size_t) * CHAR_BIT) - 1)))
        return checked_size_overflow();
    return checked_size_result_t{ceil2(v), false};
}

/// @brief  Simply dereferencing misaligned pointers can be dangerous.
template <typename at> void misaligned_store(void* ptr, at v) noexcept {
    static_assert(!std::is_reference<at>::value, "Can't store a reference");
    std::memcpy(ptr, &v, sizeof(at));
}

/// @brief  Simply dereferencing misaligned pointers can be dangerous.
template <typename at> at misaligned_load(void const* ptr) noexcept {
    static_assert(!std::is_reference<at>::value, "Can't load a reference");
    at v;
    std::memcpy(&v, ptr, sizeof(at));
    return v;
}

/// @brief  The `std::exchange` alternative for C++11.
template <typename at, typename other_at = at> at exchange(at& obj, other_at&& new_value) {
    at old_value = std::move(obj);
    obj = std::forward<other_at>(new_value);
    return old_value;
}

#if defined(USEARCH_DEFINED_CPP20)

template <typename at> void destroy_at(at* obj) { std::destroy_at(obj); }
template <typename at> void construct_at(at* obj) { std::construct_at(obj); }

#else

/// @brief  The `std::destroy_at` alternative for C++11.
template <typename at, typename sfinae_at = at>
typename std::enable_if<std::is_pod<sfinae_at>::value>::type destroy_at(at*) {}
template <typename at, typename sfinae_at = at>
typename std::enable_if<!std::is_pod<sfinae_at>::value>::type destroy_at(at* obj) {
    obj->~sfinae_at();
}

/// @brief  The `std::construct_at` alternative for C++11.
template <typename at, typename sfinae_at = at>
typename std::enable_if<std::is_pod<sfinae_at>::value>::type construct_at(at*) {}
template <typename at, typename sfinae_at = at>
typename std::enable_if<!std::is_pod<sfinae_at>::value>::type construct_at(at* obj) {
    new (obj) at();
}

#endif

/**
 *  @brief  A reference to a misaligned memory location with a specific type.
 *          It is needed to avoid Undefined Behavior when dereferencing addresses
 *          indivisible by `sizeof(at)`.
 */
template <typename at> class misaligned_ref_gt {
    using element_t = at;
    using mutable_t = typename std::remove_const<element_t>::type;
    byte_t* ptr_;

  public:
    misaligned_ref_gt(byte_t* ptr) noexcept : ptr_(ptr) {}
    operator mutable_t() const noexcept { return misaligned_load<mutable_t>(ptr_); }
    misaligned_ref_gt& operator=(mutable_t const& v) noexcept {
        misaligned_store<mutable_t>(ptr_, v);
        return *this;
    }

    void reset(byte_t* ptr) noexcept { ptr_ = ptr; }
    byte_t* ptr() const noexcept { return ptr_; }
};

/**
 *  @brief  A pointer to a misaligned memory location with a specific type.
 *          It is needed to avoid Undefined Behavior when dereferencing addresses
 *          indivisible by `sizeof(at)`.
 */
template <typename at> class misaligned_ptr_gt {
    using element_t = at;
    using mutable_t = typename std::remove_const<element_t>::type;
    byte_t* ptr_;

  public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = element_t;
    using difference_type = std::ptrdiff_t;
    using pointer = misaligned_ptr_gt<element_t>;
    using reference = misaligned_ref_gt<element_t>;

    misaligned_ptr_gt(byte_t* ptr) noexcept : ptr_(ptr) {}

    reference operator*() const noexcept { return {ptr_}; }
    reference operator[](std::size_t i) noexcept { return reference(ptr_ + i * sizeof(element_t)); }
    value_type operator[](std::size_t i) const noexcept {
        return misaligned_load<element_t>(ptr_ + i * sizeof(element_t));
    }

    misaligned_ptr_gt& operator++() noexcept {
        ptr_ += sizeof(element_t);
        return *this;
    }
    misaligned_ptr_gt& operator--() noexcept {
        ptr_ -= sizeof(element_t);
        return *this;
    }
    misaligned_ptr_gt operator++(int) noexcept {
        misaligned_ptr_gt tmp = *this;
        ++(*this);
        return tmp;
    }
    misaligned_ptr_gt operator--(int) noexcept {
        misaligned_ptr_gt tmp = *this;
        --(*this);
        return tmp;
    }
    misaligned_ptr_gt operator+(difference_type d) const noexcept {
        return misaligned_ptr_gt(ptr_ + d * sizeof(element_t));
    }
    misaligned_ptr_gt operator-(difference_type d) const noexcept {
        return misaligned_ptr_gt(ptr_ - d * sizeof(element_t));
    }
    difference_type operator-(const misaligned_ptr_gt& other) const noexcept {
        return (ptr_ - other.ptr_) / sizeof(element_t);
    }

    misaligned_ptr_gt& operator+=(difference_type d) noexcept {
        ptr_ += d * sizeof(element_t);
        return *this;
    }
    misaligned_ptr_gt& operator-=(difference_type d) noexcept {
        ptr_ -= d * sizeof(element_t);
        return *this;
    }

    bool operator==(misaligned_ptr_gt const& other) const noexcept { return ptr_ == other.ptr_; }
    bool operator!=(misaligned_ptr_gt const& other) const noexcept { return ptr_ != other.ptr_; }
    bool operator<(misaligned_ptr_gt const& other) const noexcept { return ptr_ < other.ptr_; }
    bool operator<=(misaligned_ptr_gt const& other) const noexcept { return ptr_ <= other.ptr_; }
    bool operator>(misaligned_ptr_gt const& other) const noexcept { return ptr_ > other.ptr_; }
    bool operator>=(misaligned_ptr_gt const& other) const noexcept { return ptr_ >= other.ptr_; }
};

/**
 *  @brief  Non-owning memory range view, similar to `std::span`, but for C++11.
 */
template <typename scalar_at> class span_gt {
    scalar_at* data_;
    std::size_t size_;

  public:
    span_gt() noexcept : data_(nullptr), size_(0u) {}
    span_gt(scalar_at* begin, scalar_at* end) noexcept : data_(begin), size_(end - begin) {}
    span_gt(scalar_at* begin, std::size_t size) noexcept : data_(begin), size_(size) {}
    scalar_at* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    scalar_at* begin() const noexcept { return data_; }
    scalar_at* end() const noexcept { return data_ + size_; }
    operator scalar_at*() const noexcept { return data(); }
};

/**
 *  @brief  Similar to `std::vector`, but doesn't support dynamic resizing.
 *          On the bright side, this can't throw exceptions.
 */
template <typename scalar_at, typename allocator_at = std::allocator<scalar_at>> class buffer_gt {
    scalar_at* data_;
    std::size_t size_;

  public:
    buffer_gt() noexcept : data_(nullptr), size_(0u) {}
    buffer_gt(std::size_t size) noexcept : data_(allocator_at{}.allocate(size)), size_(data_ ? size : 0u) {
        if (!std::is_trivially_default_constructible<scalar_at>::value)
            for (std::size_t i = 0; i != size_; ++i)
                construct_at(data_ + i);
    }
    ~buffer_gt() noexcept { reset(); }
    void reset() noexcept {
        if (!std::is_trivially_destructible<scalar_at>::value)
            for (std::size_t i = 0; i != size_; ++i)
                unum::usearch::destroy_at(data_ + i); //< Facing some symbol visibility/ambiguity issues
        allocator_at{}.deallocate(data_, size_);
        data_ = nullptr;
        size_ = 0;
    }
    scalar_at* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    scalar_at* begin() const noexcept { return data_; }
    scalar_at* end() const noexcept { return data_ + size_; }
    operator scalar_at*() const noexcept { return data(); }
    scalar_at& operator[](std::size_t i) noexcept { return data_[i]; }
    scalar_at const& operator[](std::size_t i) const noexcept { return data_[i]; }
    explicit operator bool() const noexcept { return data_; }
    scalar_at* release() noexcept {
        size_ = 0;
        return exchange(data_, nullptr);
    }

    buffer_gt(buffer_gt const&) = delete;
    buffer_gt& operator=(buffer_gt const&) = delete;

    buffer_gt(buffer_gt&& other) noexcept : data_(exchange(other.data_, nullptr)), size_(exchange(other.size_, 0)) {}
    buffer_gt& operator=(buffer_gt&& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        return *this;
    }
};

/**
 *  @brief  A lightweight error class for handling error messages,
 *          which are expected to be allocated in static memory.
 */
class error_t {
    char const* message_{};

  public:
    error_t() noexcept : message_(nullptr) {}
    error_t(char const* message) noexcept : message_(message) {}
    error_t& operator=(char const* message) noexcept {
        message_ = message;
        return *this;
    }

    error_t(error_t const&) = delete;
    error_t& operator=(error_t const&) = delete;
    error_t(error_t&& other) noexcept : message_(exchange(other.message_, nullptr)) {}
    error_t& operator=(error_t&& other) noexcept {
        std::swap(message_, other.message_);
        return *this;
    }

    /// @brief Checks if there was an error.
    explicit operator bool() const noexcept { return message_ != nullptr; }

    /// @brief Returns the error message.
    char const* what() const noexcept { return message_; }

    /// @brief Releases the error message, meaning the caller takes ownership.
    char const* release() noexcept { return exchange(message_, nullptr); }

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    /// @brief Destructor raises an exception if an error was recorded.
    ~error_t() noexcept(false) {
#if defined(USEARCH_DEFINED_CPP17)
        if (message_ && std::uncaught_exceptions() == 0)
#else
        if (message_ && std::uncaught_exception() == 0)
#endif
            raise();
    }

    /// @brief Throws an exception using to be caught by `try` / `catch`.
    void raise() noexcept(false) {
        if (message_)
            throw std::runtime_error(exchange(message_, nullptr));
    }
#else
    /// @brief Destructor terminates if an error was recorded.
    ~error_t() noexcept { raise(); }

    /// @brief Terminates if an error was recorded.
    void raise() noexcept {
        if (message_)
            std::terminate();
    }
#endif
};

/**
 *  @brief  Similar to `std::expected` in C++23, wraps a statement evaluation result,
 *          or an error. It's used to avoid raising exception, and gracefully propagate
 *          the error.
 *
 *  @tparam result_at The type of the expected result.
 */
template <typename result_at> struct expected_gt {
    result_at result;
    error_t error;

    operator result_at&() & {
        error.raise();
        return result;
    }
    operator result_at&&() && {
        error.raise();
        return std::move(result);
    }
    result_at const& operator*() const noexcept { return result; }
    explicit operator bool() const noexcept { return !error; }
    expected_gt failed(error_t message) noexcept {
        error = std::move(message);
        return std::move(*this);
    }
};


} // namespace usearch
} // namespace unum
