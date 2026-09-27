/**
 *  @file       punned.hpp
 *  @brief      metric_punned_t 类型擦除度量；不含 dense。
 */
#pragma once
#include <plugins/isa.hpp>
namespace unum {
namespace usearch {

/**
 *  @brief  Type-punned metric class, which unlike STL's `std::function` avoids any memory allocations.
 *          It also provides additional APIs to check, if SIMD hardware-acceleration is available.
 *          Wraps the `nk_metric_dense_punned_t` when available. The auto-vectorized backend otherwise.
 */
class metric_punned_t {
  public:
    using scalar_t = byte_t;
    using result_t = distance_punned_t;

  private:
    /// In the generalized function API all the are arguments are pointer-sized.
    using uptr_t = std::size_t;
    /// Distance function that takes two arrays and returns a scalar.
    using metric_array_array_t = result_t (*)(uptr_t, uptr_t);
    /// Distance function that takes two arrays and their length and returns a scalar.
    using metric_array_array_size_t = result_t (*)(uptr_t, uptr_t, uptr_t);
    /// Distance function that takes two arrays and some callback state and returns a scalar.
    using metric_array_array_state_t = result_t (*)(uptr_t, uptr_t, uptr_t);
    /// Distance function callback, like `metric_array_array_size_t`, but depends on member variables.
    using metric_routed_t = result_t (metric_punned_t::*)(uptr_t, uptr_t) const;

    metric_routed_t metric_routed_ = nullptr;
    uptr_t metric_ptr_ = 0;
    uptr_t metric_third_arg_ = 0;

    std::size_t dimensions_ = 0;
    metric_kind_t metric_kind_ = metric_kind_t::unknown_k;
    scalar_kind_t scalar_kind_ = scalar_kind_t::unknown_k;

#if USEARCH_USE_NUMKONG
    nk_capability_t isa_kind_ = nk_cap_serial_k;
#endif

  public:
    /**
     *  @brief  Computes the distance between two vectors of fixed length.
     *
     *  ! This is the only relevant function in the object. Everything else is just dynamic dispatch logic.
     */
    inline result_t operator()(byte_t const* a, byte_t const* b) const noexcept {
        return (this->*metric_routed_)(reinterpret_cast<uptr_t>(a), reinterpret_cast<uptr_t>(b));
    }

    inline metric_punned_t() noexcept = default;
    inline metric_punned_t(metric_punned_t const&) noexcept = default;
    inline metric_punned_t& operator=(metric_punned_t const&) noexcept = default;

    inline metric_punned_t(std::size_t dimensions, metric_kind_t metric_kind = metric_kind_t::l2sq_k,
                           scalar_kind_t scalar_kind = scalar_kind_t::f32_k) noexcept
        : metric_punned_t(builtin(dimensions, metric_kind, scalar_kind)) {}

    inline metric_punned_t(std::size_t dimensions, std::uintptr_t metric_uintptr, metric_punned_signature_t signature,
                           metric_kind_t metric_kind, scalar_kind_t scalar_kind) noexcept
        : metric_punned_t(stateless(dimensions, metric_uintptr, signature, metric_kind, scalar_kind)) {}

    /**
     *  @brief  Creates a metric of a natively supported kind, choosing the best
     *          available backend internally or from NumKong.
     *
     *  @param  dimensions      The number of elements in the input arrays.
     *  @param  metric_kind     The kind of metric to use.
     *  @param  scalar_kind     The kind of scalar to use.
     *  @return                 A metric object that can be used to compute distances between vectors.
     */
    inline static metric_punned_t builtin(std::size_t dimensions, metric_kind_t metric_kind = metric_kind_t::l2sq_k,
                                          scalar_kind_t scalar_kind = scalar_kind_t::f32_k) noexcept {
        metric_punned_t metric;
        metric.metric_routed_ = &metric_punned_t::invoke_array_array_third;
        metric.metric_ptr_ = 0;
        metric.metric_third_arg_ =
            scalar_kind == scalar_kind_t::b1x8_k ? divide_round_up<CHAR_BIT>(dimensions) : dimensions;
        metric.dimensions_ = dimensions;
        metric.metric_kind_ = metric_kind;
        metric.scalar_kind_ = scalar_kind;

        if (!metric.configure_with_numkong())
            metric.configure_with_autovec();

        return metric;
    }

#if USEARCH_USE_NUMKONG
    /**
     *  @brief  Like `builtin`, but restricts NumKong dispatch to `caps`
     *          (tests: serial-mask reference vs full CPU mask).
     */
    inline static metric_punned_t builtin_with_caps(std::size_t dimensions, metric_kind_t metric_kind,
                                                    scalar_kind_t scalar_kind, nk_capability_t caps) noexcept {
        metric_punned_t metric;
        metric.metric_routed_ = &metric_punned_t::invoke_array_array_third;
        metric.metric_ptr_ = 0;
        metric.metric_third_arg_ =
            scalar_kind == scalar_kind_t::b1x8_k ? divide_round_up<CHAR_BIT>(dimensions) : dimensions;
        metric.dimensions_ = dimensions;
        metric.metric_kind_ = metric_kind;
        metric.scalar_kind_ = scalar_kind;

        if (!metric.configure_with_numkong(caps))
            metric.configure_with_autovec();

        return metric;
    }
#endif

    /**
     *  @brief  Creates a metric using the provided function pointer for a stateless metric.
     *          So the provided ::metric_uintptr is a pointer to a function that takes two arrays
     *          and returns a scalar. If the ::signature is metric_punned_signature_t::array_array_size_k,
     *          then the third argument is the number of scalar words in the input vectors.
     *
     *  @param  dimensions      The number of elements in the input arrays.
     *  @param  metric_uintptr  The function pointer to the metric function.
     *  @param  signature       The signature of the metric function.
     *  @param  metric_kind     The kind of metric to use.
     *  @param  scalar_kind     The kind of scalar to use.
     *  @return                 A metric object that can be used to compute distances between vectors.
     */
    inline static metric_punned_t stateless(std::size_t dimensions, std::uintptr_t metric_uintptr,
                                            metric_punned_signature_t signature, metric_kind_t metric_kind,
                                            scalar_kind_t scalar_kind) noexcept {
        metric_punned_t metric;
        metric.metric_routed_ = signature == metric_punned_signature_t::array_array_k
                                    ? &metric_punned_t::invoke_array_array
                                    : &metric_punned_t::invoke_array_array_third;
        metric.metric_ptr_ = metric_uintptr;
        metric.metric_third_arg_ =
            scalar_kind == scalar_kind_t::b1x8_k ? divide_round_up<CHAR_BIT>(dimensions) : dimensions;
        metric.dimensions_ = dimensions;
        metric.metric_kind_ = metric_kind;
        metric.scalar_kind_ = scalar_kind;
        return metric;
    }

    /**
     *  @brief  Creates a metric using the provided function pointer for a stateful metric.
     *          The third argument is the state that will be passed to the metric function.
     *
     *  @param  dimensions      The number of elements in the input arrays.
     *  @param  metric_uintptr  The function pointer to the metric function.
     *  @param  metric_state    The state to pass to the metric function.
     *  @param  metric_kind     The kind of metric to use.
     *  @param  scalar_kind     The kind of scalar to use.
     *  @return                 A metric object that can be used to compute distances between vectors.
     */
    inline static metric_punned_t stateful( //
        std::size_t dimensions, std::uintptr_t metric_uintptr, std::uintptr_t metric_state,
        metric_kind_t metric_kind = metric_kind_t::unknown_k,
        scalar_kind_t scalar_kind = scalar_kind_t::unknown_k) noexcept {
        metric_punned_t metric;
        metric.metric_routed_ = &metric_punned_t::invoke_array_array_third;
        metric.metric_ptr_ = metric_uintptr;
        metric.metric_third_arg_ = metric_state;
        metric.dimensions_ = dimensions;
        metric.metric_kind_ = metric_kind;
        metric.scalar_kind_ = scalar_kind;
        return metric;
    }

    inline std::size_t dimensions() const noexcept { return dimensions_; }
    inline metric_kind_t metric_kind() const noexcept { return metric_kind_; }
    inline scalar_kind_t scalar_kind() const noexcept { return scalar_kind_; }
    inline explicit operator bool() const noexcept { return metric_routed_ && metric_ptr_; }

    /**
     *  @brief  Checks if we've failed to initialize the metric with provided arguments.
     *
     *  It's different from `operator bool()` when it comes to explicitly uninitialized metrics.
     *  It's a common case, where a NULL state is created only to be overwritten later, when
     *  we recover an old index state from a file or a network.
     */
    inline bool missing() const noexcept { return !bool(*this) && metric_kind_ != metric_kind_t::unknown_k; }

    inline char const* isa_name() const noexcept {
        if (!*this)
            return "uninitialized";
#if USEARCH_USE_NUMKONG
        return capability_name(isa_kind_);
#else
        return "serial";
#endif
    }

    inline std::size_t bytes_per_vector() const noexcept {
        return divide_round_up<CHAR_BIT>(dimensions_ * bits_per_scalar(scalar_kind_));
    }

    inline std::size_t scalar_words() const noexcept {
        return divide_round_up(dimensions_ * bits_per_scalar(scalar_kind_), bits_per_scalar_word(scalar_kind_));
    }

  private:
#if USEARCH_USE_NUMKONG
    /**
     *  @brief  Typed invoke template for NumKong kernels.
     *
     *  The accumulator type and IP-reversal flag are selected once in `configure_with_numkong`
     *  and baked into the member-function pointer stored in `metric_routed_`.
     */
    template <typename accumulator_at, bool reverse_ak>
#if defined(USEARCH_DEFINED_CLANG) || defined(USEARCH_DEFINED_GCC)
    __attribute__((no_sanitize("all")))
#endif
    result_t invoke_numkong(uptr_t a, uptr_t b) const noexcept {
        accumulator_at result = 0;
        auto function_pointer = (nk_metric_dense_punned_t)(metric_ptr_);
        function_pointer(reinterpret_cast<void const*>(a), reinterpret_cast<void const*>(b), metric_third_arg_,
                         &result);
        return reverse_ak ? (result_t)(1 - (result_t)result) : (result_t)result;
    }

    /// Shorthand: casts an `invoke_numkong` instantiation to the opaque `metric_routed_t` type.
    template <typename accumulator_at, bool reverse_ak> //
    static metric_routed_t numkong_routed() noexcept {
        return reinterpret_cast<metric_routed_t>(&metric_punned_t::invoke_numkong<accumulator_at, reverse_ak>);
    }

    bool configure_with_numkong(nk_capability_t simd_caps) noexcept {
        nk_kernel_kind_t kind = (nk_kernel_kind_t)0;
        switch (metric_kind_) {
        case metric_kind_t::ip_k: kind = nk_kernel_dot_k; break;
        case metric_kind_t::cos_k: kind = nk_kernel_angular_k; break;
        case metric_kind_t::l2sq_k: kind = nk_kernel_sqeuclidean_k; break;
        case metric_kind_t::hamming_k: kind = nk_kernel_hamming_k; break;
        case metric_kind_t::tanimoto_k: kind = nk_kernel_jaccard_k; break;
        case metric_kind_t::jaccard_k: kind = nk_kernel_jaccard_k; break;
        default: return false;
        }
        nk_dtype_t datatype = scalar_kind_to_nk_dtype(scalar_kind_);
        if (datatype == (nk_dtype_t)0)
            return false;
        nk_metric_dense_punned_t simd_metric = NULL;
        nk_capability_t simd_kind = nk_cap_any_k;
        nk_find_kernel_punned(kind, datatype, simd_caps, (nk_kernel_punned_t*)&simd_metric, &simd_kind);
        // Typed miss stubs are non-null with capability 0 — must not treat them as a real kernel.
        if (simd_metric == nullptr || simd_kind == 0)
            return false;

        std::memcpy(&metric_ptr_, &simd_metric, sizeof(simd_metric));

        // Select the typed invoke variant based on the kernel's output dtype to avoid per-call branching.
        // The output type depends on both scalar_kind and metric_kind (e.g. i8 dot->i32, i8 l2sq->u32, i8 cos->f32).
        nk_dtype_t out_dtype = nk_kernel_output_dtype(kind, datatype);
        bool is_ip = (metric_kind_ == metric_kind_t::ip_k);
        switch (out_dtype) {
        case nk_f64_k:
            metric_routed_ = is_ip ? numkong_routed<nk_f64_t, true>() : numkong_routed<nk_f64_t, false>();
            break;
        case nk_f32_k:
            metric_routed_ = is_ip ? numkong_routed<nk_f32_t, true>() : numkong_routed<nk_f32_t, false>();
            break;
        case nk_i32_k:
            metric_routed_ = is_ip ? numkong_routed<std::int32_t, true>() : numkong_routed<std::int32_t, false>();
            break;
        case nk_u32_k:
            metric_routed_ = is_ip ? numkong_routed<std::uint32_t, true>() : numkong_routed<std::uint32_t, false>();
            break;
        default: metric_routed_ = is_ip ? numkong_routed<nk_f64_t, true>() : numkong_routed<nk_f64_t, false>(); break;
        }
        isa_kind_ = simd_kind;

        // NumKong binary-set kernels (Hamming, Jaccard/Tanimoto) expect the third argument
        // to be the number of dimensions (bits), not the number of bytes.
        if (scalar_kind_ == scalar_kind_t::b1x8_k)
            metric_third_arg_ = dimensions_;

        return true;
    }

    bool configure_with_numkong() noexcept { return configure_with_numkong(nk_cached_capabilities()); }
#else
    bool configure_with_numkong() noexcept { return false; }
#endif
    result_t invoke_array_array_third(uptr_t a, uptr_t b) const noexcept {
        auto function_pointer = (metric_array_array_size_t)(metric_ptr_);
        result_t result = function_pointer(a, b, metric_third_arg_);
        return result;
    }
    result_t invoke_array_array(uptr_t a, uptr_t b) const noexcept {
        auto function_pointer = (metric_array_array_t)(metric_ptr_);
        result_t result = function_pointer(a, b);
        return result;
    }
    void configure_with_autovec() noexcept {
        switch (metric_kind_) {
        case metric_kind_t::ip_k: {
            switch (scalar_kind_) {
            case scalar_kind_t::f64_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_ip_gt<f64_t>>; break;
            case scalar_kind_t::f32_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_ip_gt<f32_t>>; break;
            case scalar_kind_t::bf16_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_ip_gt<bf16_t, f32_t>>; break;
            case scalar_kind_t::f16_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_ip_gt<f16_t, f32_t>>; break;
            case scalar_kind_t::e5m2_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_ip_gt<e5m2_t, f32_t>>; break;
            case scalar_kind_t::e4m3_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_ip_gt<e4m3_t, f32_t>>; break;
            case scalar_kind_t::e3m2_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_ip_gt<e3m2_t, f32_t>>; break;
            case scalar_kind_t::e2m3_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_ip_gt<e2m3_t, f32_t>>; break;
            case scalar_kind_t::i8_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_ip_gt<i8_t, f32_t>>; break;
            case scalar_kind_t::u8_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_ip_gt<u8_t, f32_t>>; break;
            default: metric_ptr_ = 0; break;
            }
            break;
        }
        case metric_kind_t::cos_k: {
            switch (scalar_kind_) {
            case scalar_kind_t::f64_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_cos_gt<f64_t>>; break;
            case scalar_kind_t::f32_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_cos_gt<f32_t>>; break;
            case scalar_kind_t::bf16_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_cos_gt<bf16_t, f32_t>>; break;
            case scalar_kind_t::f16_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_cos_gt<f16_t, f32_t>>; break;
            case scalar_kind_t::e5m2_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_cos_gt<e5m2_t, f32_t>>; break;
            case scalar_kind_t::e4m3_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_cos_gt<e4m3_t, f32_t>>; break;
            case scalar_kind_t::e3m2_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_cos_gt<e3m2_t, f32_t>>; break;
            case scalar_kind_t::e2m3_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_cos_gt<e2m3_t, f32_t>>; break;
            case scalar_kind_t::i8_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_cos_i8_t>; break;
            case scalar_kind_t::u8_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_cos_u8_t>; break;
            default: metric_ptr_ = 0; break;
            }
            break;
        }
        case metric_kind_t::l2sq_k: {
            switch (scalar_kind_) {
            case scalar_kind_t::f64_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_l2sq_gt<f64_t>>; break;
            case scalar_kind_t::f32_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_l2sq_gt<f32_t>>; break;
            case scalar_kind_t::bf16_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_l2sq_gt<bf16_t, f32_t>>; break;
            case scalar_kind_t::f16_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_l2sq_gt<f16_t, f32_t>>; break;
            case scalar_kind_t::e5m2_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_l2sq_gt<e5m2_t, f32_t>>; break;
            case scalar_kind_t::e4m3_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_l2sq_gt<e4m3_t, f32_t>>; break;
            case scalar_kind_t::e3m2_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_l2sq_gt<e3m2_t, f32_t>>; break;
            case scalar_kind_t::e2m3_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_l2sq_gt<e2m3_t, f32_t>>; break;
            case scalar_kind_t::i8_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_l2sq_i8_t>; break;
            case scalar_kind_t::u8_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_l2sq_u8_t>; break;
            default: metric_ptr_ = 0; break;
            }
            break;
        }
        case metric_kind_t::pearson_k: {
            switch (scalar_kind_) {
            case scalar_kind_t::f64_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_pearson_gt<f64_t>>; break;
            case scalar_kind_t::f32_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_pearson_gt<f32_t>>; break;
            case scalar_kind_t::bf16_k:
                metric_ptr_ = (uptr_t)&equidimensional_<metric_pearson_gt<bf16_t, f32_t>>;
                break;
            case scalar_kind_t::f16_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_pearson_gt<f16_t, f32_t>>; break;
            case scalar_kind_t::e5m2_k:
                metric_ptr_ = (uptr_t)&equidimensional_<metric_pearson_gt<e5m2_t, f32_t>>;
                break;
            case scalar_kind_t::e4m3_k:
                metric_ptr_ = (uptr_t)&equidimensional_<metric_pearson_gt<e4m3_t, f32_t>>;
                break;
            case scalar_kind_t::e3m2_k:
                metric_ptr_ = (uptr_t)&equidimensional_<metric_pearson_gt<e3m2_t, f32_t>>;
                break;
            case scalar_kind_t::e2m3_k:
                metric_ptr_ = (uptr_t)&equidimensional_<metric_pearson_gt<e2m3_t, f32_t>>;
                break;
            case scalar_kind_t::i8_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_pearson_gt<i8_t, f32_t>>; break;
            case scalar_kind_t::u8_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_pearson_gt<u8_t, f32_t>>; break;
            default: metric_ptr_ = 0; break;
            }
            break;
        }
        case metric_kind_t::haversine_k: {
            switch (scalar_kind_) {
            case scalar_kind_t::f64_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_haversine_gt<f64_t>>; break;
            case scalar_kind_t::f32_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_haversine_gt<f32_t>>; break;
            default: metric_ptr_ = 0; break;
            }
            break;
        }
        case metric_kind_t::divergence_k: {
            switch (scalar_kind_) {
            case scalar_kind_t::f64_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_divergence_gt<f64_t>>; break;
            case scalar_kind_t::f32_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_divergence_gt<f32_t>>; break;
            case scalar_kind_t::bf16_k:
                metric_ptr_ = (uptr_t)&equidimensional_<metric_divergence_gt<bf16_t, f32_t>>;
                break;
            case scalar_kind_t::f16_k:
                metric_ptr_ = (uptr_t)&equidimensional_<metric_divergence_gt<f16_t, f32_t>>;
                break;
            case scalar_kind_t::e5m2_k:
                metric_ptr_ = (uptr_t)&equidimensional_<metric_divergence_gt<e5m2_t, f32_t>>;
                break;
            case scalar_kind_t::e4m3_k:
                metric_ptr_ = (uptr_t)&equidimensional_<metric_divergence_gt<e4m3_t, f32_t>>;
                break;
            case scalar_kind_t::e3m2_k:
                metric_ptr_ = (uptr_t)&equidimensional_<metric_divergence_gt<e3m2_t, f32_t>>;
                break;
            case scalar_kind_t::e2m3_k:
                metric_ptr_ = (uptr_t)&equidimensional_<metric_divergence_gt<e2m3_t, f32_t>>;
                break;
            default: metric_ptr_ = 0; break;
            }
            break;
        }
        case metric_kind_t::jaccard_k: // Equivalent to Tanimoto
        case metric_kind_t::tanimoto_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_tanimoto_gt<b1x8_t>>; break;
        case metric_kind_t::hamming_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_hamming_gt<b1x8_t>>; break;
        case metric_kind_t::sorensen_k: metric_ptr_ = (uptr_t)&equidimensional_<metric_sorensen_gt<b1x8_t>>; break;
        default: return;
        }
    }

    template <typename typed_at>
    inline static result_t equidimensional_(uptr_t a, uptr_t b, uptr_t a_dimensions) noexcept {
        using scalar_t = typename typed_at::scalar_t;
        return static_cast<result_t>(typed_at{}((scalar_t const*)a, (scalar_t const*)b, a_dimensions));
    }
};

/* Allow complaining about vectorization after this point. */
#if defined(USEARCH_DEFINED_CLANG)
#pragma clang diagnostic pop
#endif

} // namespace usearch
} // namespace unum
