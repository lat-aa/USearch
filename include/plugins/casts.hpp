/**
 *  @file       casts.hpp
 *  @brief      数组类型转换与 casts_punned_t。
 */
#pragma once
#include <plugins/alloc.hpp>
#include <plugins/floats.hpp>
namespace unum {
namespace usearch {

/**
 *  @brief  Utility class used to cast arrays of one scalar type to another,
 *          avoiding unnecessary conversions.
 */
template <typename from_scalar_at, typename to_scalar_at> struct cast_gt {
    static bool try_(byte_t const* input, std::size_t dim, byte_t* output) noexcept {
        from_scalar_at const* typed_input = reinterpret_cast<from_scalar_at const*>(input);
        to_scalar_at* typed_output = reinterpret_cast<to_scalar_at*>(output);
        auto converter = [](from_scalar_at from) { return to_scalar_at(from); };
        std::transform(typed_input, typed_input + dim, typed_output, converter);
        return true;
    }
};

template <> struct cast_gt<f32_t, f32_t> {
    static bool try_(byte_t const*, std::size_t, byte_t*) noexcept { return false; }
};

template <> struct cast_gt<f64_t, f64_t> {
    static bool try_(byte_t const*, std::size_t, byte_t*) noexcept { return false; }
};

template <> struct cast_gt<f16_bits_t, f16_bits_t> {
    static bool try_(byte_t const*, std::size_t, byte_t*) noexcept { return false; }
};

template <> struct cast_gt<bf16_bits_t, bf16_bits_t> {
    static bool try_(byte_t const*, std::size_t, byte_t*) noexcept { return false; }
};

template <> struct cast_gt<i8_t, i8_t> {
    static bool try_(byte_t const*, std::size_t, byte_t*) noexcept { return false; }
};

template <> struct cast_gt<u8_t, u8_t> {
    static bool try_(byte_t const*, std::size_t, byte_t*) noexcept { return false; }
};

template <> struct cast_gt<b1x8_t, b1x8_t> {
    static bool try_(byte_t const*, std::size_t, byte_t*) noexcept { return false; }
};

template <typename from_scalar_at> struct cast_to_b1x8_gt {
    inline static bool try_(byte_t const* input, std::size_t dim, byte_t* output) noexcept {
        from_scalar_at const* typed_input = reinterpret_cast<from_scalar_at const*>(input);
        unsigned char* typed_output = reinterpret_cast<unsigned char*>(output);
        std::memset(typed_output, 0, dim / CHAR_BIT);
        for (std::size_t i = 0; i != dim; ++i)
            // Converting from scalar types to boolean isn't trivial and depends on the type.
            // The most common case is to consider all positive values as `true` and all others as `false`.
            //  - `bool(0.00001f)` converts to 1
            //  - `bool(-0.00001f)` converts to 1
            //  - `bool(0)` converts to 0
            //  - `bool(-0)` converts to 0
            //  - `bool(std::numeric_limits<float>::infinity())` converts to 1
            //  - `bool(std::numeric_limits<float>::epsilon())` converts to 1
            //  - `bool(std::numeric_limits<float>::signaling_NaN())` converts to 1
            //  - `bool(std::numeric_limits<float>::denorm_min())` converts to 1
            typed_output[i / CHAR_BIT] |= bool(typed_input[i] > 0) ? (128 >> (i & (CHAR_BIT - 1))) : 0;
        return true;
    }
};

template <typename to_scalar_at> struct cast_from_b1x8_gt {
    static bool try_(byte_t const* input, std::size_t dim, byte_t* output) noexcept {
        unsigned char const* typed_input = reinterpret_cast<unsigned char const*>(input);
        to_scalar_at* typed_output = reinterpret_cast<to_scalar_at*>(output);
        for (std::size_t i = 0; i != dim; ++i)
            // We can't entirely reconstruct the original scalar type from a boolean.
            // The simplest variant would be to map set bits to ones, and unset bits to zeros.
            typed_output[i] = bool(typed_input[i / CHAR_BIT] & (128 >> (i & (CHAR_BIT - 1))));
        return true;
    }
};

template <typename from_scalar_at> struct cast_to_i8_gt {
    inline static bool try_(byte_t const* input, std::size_t dim, byte_t* output) noexcept {
        from_scalar_at const* typed_input = reinterpret_cast<from_scalar_at const*>(input);
        std::int8_t* typed_output = reinterpret_cast<std::int8_t*>(output);
        // Unlike other casting mechanisms, switching to small range integers is a two step procedure.
        // First we want to estimate the magnitude of the vector to scale into [-1.0, 1.0] interval,
        // instead of clamping. And then we scale the values into the [-127, 127] range.
        // ! This makes an assumption, that the distance metric is dot-product-like, which may not
        // ! be true in many cases, so it's recommended to avoid automatic casting from floats to
        // ! integers.
        double magnitude = 0.0;
        for (std::size_t i = 0; i != dim; ++i)
            magnitude += (double)typed_input[i] * (double)typed_input[i];
        magnitude = std::sqrt(magnitude);
        // `!(x > 0)` also catches NaN; cast-to-int of NaN is UB.
        if (!(magnitude > 0.0)) {
            std::fill_n(typed_output, dim, std::int8_t{0});
            return true;
        }
        for (std::size_t i = 0; i != dim; ++i)
            typed_output[i] =
                static_cast<std::int8_t>(usearch::clamp<double>(typed_input[i] * 127.0 / magnitude, -127.0, 127.0));
        return true;
    }
};

template <typename to_scalar_at> struct cast_from_i8_gt {
    static bool try_(byte_t const* input, std::size_t dim, byte_t* output) noexcept {
        std::int8_t const* typed_input = reinterpret_cast<std::int8_t const*>(input);
        to_scalar_at* typed_output = reinterpret_cast<to_scalar_at*>(output);
        for (std::size_t i = 0; i != dim; ++i)
            typed_output[i] = static_cast<to_scalar_at>(typed_input[i]) / 127.f;
        return true;
    }
};

template <typename from_scalar_at> struct cast_to_u8_gt {
    inline static bool try_(byte_t const* input, std::size_t dim, byte_t* output) noexcept {
        from_scalar_at const* typed_input = reinterpret_cast<from_scalar_at const*>(input);
        std::uint8_t* typed_output = reinterpret_cast<std::uint8_t*>(output);
        double magnitude = 0.0;
        for (std::size_t i = 0; i != dim; ++i)
            magnitude += (double)typed_input[i] * (double)typed_input[i];
        magnitude = std::sqrt(magnitude);
        // `!(x > 0)` also catches NaN; cast-to-int of NaN is UB.
        if (!(magnitude > 0.0)) {
            std::fill_n(typed_output, dim, std::uint8_t{0});
            return true;
        }
        for (std::size_t i = 0; i != dim; ++i)
            typed_output[i] =
                static_cast<std::uint8_t>(usearch::clamp<double>(typed_input[i] * 255.0 / magnitude, 0.0, 255.0));
        return true;
    }
};

template <typename to_scalar_at> struct cast_from_u8_gt {
    static bool try_(byte_t const* input, std::size_t dim, byte_t* output) noexcept {
        std::uint8_t const* typed_input = reinterpret_cast<std::uint8_t const*>(input);
        to_scalar_at* typed_output = reinterpret_cast<to_scalar_at*>(output);
        for (std::size_t i = 0; i != dim; ++i)
            typed_output[i] = static_cast<to_scalar_at>(typed_input[i]) / 255.f;
        return true;
    }
};

template <> struct cast_gt<i8_t, f16_bits_t> : public cast_from_i8_gt<f16_t> {};
template <> struct cast_gt<i8_t, bf16_bits_t> : public cast_from_i8_gt<bf16_t> {};
template <> struct cast_gt<i8_t, f32_t> : public cast_from_i8_gt<f32_t> {};
template <> struct cast_gt<i8_t, f64_t> : public cast_from_i8_gt<f64_t> {};

template <> struct cast_gt<f16_bits_t, i8_t> : public cast_to_i8_gt<f16_t> {};
template <> struct cast_gt<bf16_bits_t, i8_t> : public cast_to_i8_gt<bf16_t> {};
template <> struct cast_gt<f32_t, i8_t> : public cast_to_i8_gt<f32_t> {};
template <> struct cast_gt<f64_t, i8_t> : public cast_to_i8_gt<f64_t> {};

template <> struct cast_gt<b1x8_t, f16_bits_t> : public cast_from_b1x8_gt<f16_t> {};
template <> struct cast_gt<b1x8_t, bf16_bits_t> : public cast_from_b1x8_gt<bf16_t> {};
template <> struct cast_gt<b1x8_t, f32_t> : public cast_from_b1x8_gt<f32_t> {};
template <> struct cast_gt<b1x8_t, f64_t> : public cast_from_b1x8_gt<f64_t> {};

template <> struct cast_gt<f16_bits_t, b1x8_t> : public cast_to_b1x8_gt<f16_t> {};
template <> struct cast_gt<bf16_bits_t, b1x8_t> : public cast_to_b1x8_gt<bf16_t> {};
template <> struct cast_gt<f32_t, b1x8_t> : public cast_to_b1x8_gt<f32_t> {};
template <> struct cast_gt<f64_t, b1x8_t> : public cast_to_b1x8_gt<f64_t> {};

template <> struct cast_gt<b1x8_t, i8_t> : public cast_from_b1x8_gt<i8_t> {};
template <> struct cast_gt<i8_t, b1x8_t> : public cast_to_b1x8_gt<i8_t> {};

template <typename from_at, typename to_at> struct cast_through_f32_gt {
    static bool try_(byte_t const* input, std::size_t dim, byte_t* output) noexcept {
        from_at const* in = reinterpret_cast<from_at const*>(input);
        to_at* out = reinterpret_cast<to_at*>(output);
        for (std::size_t i = 0; i != dim; ++i)
            out[i] = to_at(float(in[i]));
        return true;
    }
};

template <> struct cast_gt<e5m2_bits_t, e5m2_bits_t> {
    static bool try_(byte_t const*, std::size_t, byte_t*) noexcept { return false; }
};
template <> struct cast_gt<e5m2_bits_t, f32_t> : public cast_through_f32_gt<e5m2_t, f32_t> {};
template <> struct cast_gt<f32_t, e5m2_bits_t> : public cast_through_f32_gt<f32_t, e5m2_t> {};
template <> struct cast_gt<e5m2_bits_t, f64_t> : public cast_through_f32_gt<e5m2_t, f64_t> {};
template <> struct cast_gt<f64_t, e5m2_bits_t> : public cast_through_f32_gt<f64_t, e5m2_t> {};
template <> struct cast_gt<e5m2_bits_t, f16_bits_t> : public cast_through_f32_gt<e5m2_t, f16_t> {};
template <> struct cast_gt<f16_bits_t, e5m2_bits_t> : public cast_through_f32_gt<f16_t, e5m2_t> {};
template <> struct cast_gt<e5m2_bits_t, bf16_bits_t> : public cast_through_f32_gt<e5m2_t, bf16_t> {};
template <> struct cast_gt<bf16_bits_t, e5m2_bits_t> : public cast_through_f32_gt<bf16_t, e5m2_t> {};
template <> struct cast_gt<e5m2_bits_t, i8_t> : public cast_to_i8_gt<e5m2_t> {};
template <> struct cast_gt<i8_t, e5m2_bits_t> : public cast_from_i8_gt<e5m2_t> {};
template <> struct cast_gt<e5m2_bits_t, b1x8_t> : public cast_to_b1x8_gt<e5m2_t> {};
template <> struct cast_gt<b1x8_t, e5m2_bits_t> : public cast_from_b1x8_gt<e5m2_t> {};

template <> struct cast_gt<e4m3_bits_t, e4m3_bits_t> {
    static bool try_(byte_t const*, std::size_t, byte_t*) noexcept { return false; }
};
template <> struct cast_gt<e4m3_bits_t, f32_t> : public cast_through_f32_gt<e4m3_t, f32_t> {};
template <> struct cast_gt<f32_t, e4m3_bits_t> : public cast_through_f32_gt<f32_t, e4m3_t> {};
template <> struct cast_gt<e4m3_bits_t, f64_t> : public cast_through_f32_gt<e4m3_t, f64_t> {};
template <> struct cast_gt<f64_t, e4m3_bits_t> : public cast_through_f32_gt<f64_t, e4m3_t> {};
template <> struct cast_gt<e4m3_bits_t, f16_bits_t> : public cast_through_f32_gt<e4m3_t, f16_t> {};
template <> struct cast_gt<f16_bits_t, e4m3_bits_t> : public cast_through_f32_gt<f16_t, e4m3_t> {};
template <> struct cast_gt<e4m3_bits_t, bf16_bits_t> : public cast_through_f32_gt<e4m3_t, bf16_t> {};
template <> struct cast_gt<bf16_bits_t, e4m3_bits_t> : public cast_through_f32_gt<bf16_t, e4m3_t> {};
template <> struct cast_gt<e4m3_bits_t, i8_t> : public cast_to_i8_gt<e4m3_t> {};
template <> struct cast_gt<i8_t, e4m3_bits_t> : public cast_from_i8_gt<e4m3_t> {};
template <> struct cast_gt<e4m3_bits_t, b1x8_t> : public cast_to_b1x8_gt<e4m3_t> {};
template <> struct cast_gt<b1x8_t, e4m3_bits_t> : public cast_from_b1x8_gt<e4m3_t> {};

template <> struct cast_gt<e2m3_bits_t, e2m3_bits_t> {
    static bool try_(byte_t const*, std::size_t, byte_t*) noexcept { return false; }
};
template <> struct cast_gt<e2m3_bits_t, f32_t> : public cast_through_f32_gt<e2m3_t, f32_t> {};
template <> struct cast_gt<f32_t, e2m3_bits_t> : public cast_through_f32_gt<f32_t, e2m3_t> {};
template <> struct cast_gt<e2m3_bits_t, f64_t> : public cast_through_f32_gt<e2m3_t, f64_t> {};
template <> struct cast_gt<f64_t, e2m3_bits_t> : public cast_through_f32_gt<f64_t, e2m3_t> {};
template <> struct cast_gt<e2m3_bits_t, f16_bits_t> : public cast_through_f32_gt<e2m3_t, f16_t> {};
template <> struct cast_gt<f16_bits_t, e2m3_bits_t> : public cast_through_f32_gt<f16_t, e2m3_t> {};
template <> struct cast_gt<e2m3_bits_t, bf16_bits_t> : public cast_through_f32_gt<e2m3_t, bf16_t> {};
template <> struct cast_gt<bf16_bits_t, e2m3_bits_t> : public cast_through_f32_gt<bf16_t, e2m3_t> {};
template <> struct cast_gt<e2m3_bits_t, i8_t> : public cast_to_i8_gt<e2m3_t> {};
template <> struct cast_gt<i8_t, e2m3_bits_t> : public cast_from_i8_gt<e2m3_t> {};
template <> struct cast_gt<e2m3_bits_t, b1x8_t> : public cast_to_b1x8_gt<e2m3_t> {};
template <> struct cast_gt<b1x8_t, e2m3_bits_t> : public cast_from_b1x8_gt<e2m3_t> {};

template <> struct cast_gt<e3m2_bits_t, e3m2_bits_t> {
    static bool try_(byte_t const*, std::size_t, byte_t*) noexcept { return false; }
};
template <> struct cast_gt<e3m2_bits_t, f32_t> : public cast_through_f32_gt<e3m2_t, f32_t> {};
template <> struct cast_gt<f32_t, e3m2_bits_t> : public cast_through_f32_gt<f32_t, e3m2_t> {};
template <> struct cast_gt<e3m2_bits_t, f64_t> : public cast_through_f32_gt<e3m2_t, f64_t> {};
template <> struct cast_gt<f64_t, e3m2_bits_t> : public cast_through_f32_gt<f64_t, e3m2_t> {};
template <> struct cast_gt<e3m2_bits_t, f16_bits_t> : public cast_through_f32_gt<e3m2_t, f16_t> {};
template <> struct cast_gt<f16_bits_t, e3m2_bits_t> : public cast_through_f32_gt<f16_t, e3m2_t> {};
template <> struct cast_gt<e3m2_bits_t, bf16_bits_t> : public cast_through_f32_gt<e3m2_t, bf16_t> {};
template <> struct cast_gt<bf16_bits_t, e3m2_bits_t> : public cast_through_f32_gt<bf16_t, e3m2_t> {};
template <> struct cast_gt<e3m2_bits_t, i8_t> : public cast_to_i8_gt<e3m2_t> {};
template <> struct cast_gt<i8_t, e3m2_bits_t> : public cast_from_i8_gt<e3m2_t> {};
template <> struct cast_gt<e3m2_bits_t, b1x8_t> : public cast_to_b1x8_gt<e3m2_t> {};
template <> struct cast_gt<b1x8_t, e3m2_bits_t> : public cast_from_b1x8_gt<e3m2_t> {};

template <> struct cast_gt<u8_t, f16_bits_t> : public cast_from_u8_gt<f16_t> {};
template <> struct cast_gt<u8_t, bf16_bits_t> : public cast_from_u8_gt<bf16_t> {};
template <> struct cast_gt<u8_t, f32_t> : public cast_from_u8_gt<f32_t> {};
template <> struct cast_gt<u8_t, f64_t> : public cast_from_u8_gt<f64_t> {};
template <> struct cast_gt<f16_bits_t, u8_t> : public cast_to_u8_gt<f16_t> {};
template <> struct cast_gt<bf16_bits_t, u8_t> : public cast_to_u8_gt<bf16_t> {};
template <> struct cast_gt<f32_t, u8_t> : public cast_to_u8_gt<f32_t> {};
template <> struct cast_gt<f64_t, u8_t> : public cast_to_u8_gt<f64_t> {};
template <> struct cast_gt<b1x8_t, u8_t> : public cast_from_b1x8_gt<u8_t> {};
template <> struct cast_gt<u8_t, b1x8_t> : public cast_to_b1x8_gt<u8_t> {};
template <> struct cast_gt<e5m2_bits_t, u8_t> : public cast_to_u8_gt<e5m2_t> {};
template <> struct cast_gt<u8_t, e5m2_bits_t> : public cast_from_u8_gt<e5m2_t> {};
template <> struct cast_gt<e4m3_bits_t, u8_t> : public cast_to_u8_gt<e4m3_t> {};
template <> struct cast_gt<u8_t, e4m3_bits_t> : public cast_from_u8_gt<e4m3_t> {};
template <> struct cast_gt<e2m3_bits_t, u8_t> : public cast_to_u8_gt<e2m3_t> {};
template <> struct cast_gt<u8_t, e2m3_bits_t> : public cast_from_u8_gt<e2m3_t> {};
template <> struct cast_gt<e3m2_bits_t, u8_t> : public cast_to_u8_gt<e3m2_t> {};
template <> struct cast_gt<u8_t, e3m2_bits_t> : public cast_from_u8_gt<e3m2_t> {};
template <> struct cast_gt<i8_t, u8_t> : public cast_to_u8_gt<i8_t> {};
template <> struct cast_gt<u8_t, i8_t> : public cast_from_u8_gt<i8_t> {};

/**
 *  @brief  Type-punned array casting function.
 *          Arguments: input buffer, bytes in input buffer, output buffer.
 *          Returns `true` if the casting was performed successfully, `false` otherwise.
 */
using cast_punned_t = bool (*)(byte_t const*, std::size_t, byte_t*);

/**
 *  @brief  A collection of casting functions for typical vector types.
 *          Covers to/from conversions for boolean, integer, half-precision,
 *          single-precision, and double-precision scalars.
 */
struct casts_punned_t {
    struct group_t {
        cast_punned_t f64{};
        cast_punned_t f32{};
        cast_punned_t bf16{};
        cast_punned_t f16{};
        cast_punned_t e5m2{};
        cast_punned_t e4m3{};
        cast_punned_t e3m2{};
        cast_punned_t e2m3{};
        cast_punned_t i8{};
        cast_punned_t u8{};
        cast_punned_t b1x8{};

        cast_punned_t operator[](scalar_kind_t scalar_kind) const noexcept {
            switch (scalar_kind) {
            case scalar_kind_t::f64_k: return f64;
            case scalar_kind_t::f32_k: return f32;
            case scalar_kind_t::bf16_k: return bf16;
            case scalar_kind_t::f16_k: return f16;
            case scalar_kind_t::e5m2_k: return e5m2;
            case scalar_kind_t::e4m3_k: return e4m3;
            case scalar_kind_t::e3m2_k: return e3m2;
            case scalar_kind_t::e2m3_k: return e2m3;
            case scalar_kind_t::i8_k: return i8;
            case scalar_kind_t::u8_k: return u8;
            case scalar_kind_t::b1x8_k: return b1x8;
            default: return nullptr;
            }
        }

    } from, to;

    template <typename scalar_at> static casts_punned_t make() noexcept {
        casts_punned_t result;

        result.from.f64 = &cast_gt<f64_t, scalar_at>::try_;
        result.from.f32 = &cast_gt<f32_t, scalar_at>::try_;
        result.from.bf16 = &cast_gt<bf16_t, scalar_at>::try_;
        result.from.f16 = &cast_gt<f16_t, scalar_at>::try_;
        result.from.e5m2 = &cast_gt<e5m2_t, scalar_at>::try_;
        result.from.e4m3 = &cast_gt<e4m3_t, scalar_at>::try_;
        result.from.e3m2 = &cast_gt<e3m2_t, scalar_at>::try_;
        result.from.e2m3 = &cast_gt<e2m3_t, scalar_at>::try_;
        result.from.i8 = &cast_gt<i8_t, scalar_at>::try_;
        result.from.u8 = &cast_gt<u8_t, scalar_at>::try_;
        result.from.b1x8 = &cast_gt<b1x8_t, scalar_at>::try_;

        result.to.f64 = &cast_gt<scalar_at, f64_t>::try_;
        result.to.f32 = &cast_gt<scalar_at, f32_t>::try_;
        result.to.bf16 = &cast_gt<scalar_at, bf16_t>::try_;
        result.to.f16 = &cast_gt<scalar_at, f16_t>::try_;
        result.to.e5m2 = &cast_gt<scalar_at, e5m2_t>::try_;
        result.to.e4m3 = &cast_gt<scalar_at, e4m3_t>::try_;
        result.to.e3m2 = &cast_gt<scalar_at, e3m2_t>::try_;
        result.to.e2m3 = &cast_gt<scalar_at, e2m3_t>::try_;
        result.to.i8 = &cast_gt<scalar_at, i8_t>::try_;
        result.to.u8 = &cast_gt<scalar_at, u8_t>::try_;
        result.to.b1x8 = &cast_gt<scalar_at, b1x8_t>::try_;

        return result;
    }

    static casts_punned_t make(scalar_kind_t scalar_kind) noexcept {
        switch (scalar_kind) {
        case scalar_kind_t::f64_k: return casts_punned_t::make<f64_t>();
        case scalar_kind_t::f32_k: return casts_punned_t::make<f32_t>();
        case scalar_kind_t::bf16_k: return casts_punned_t::make<bf16_t>();
        case scalar_kind_t::f16_k: return casts_punned_t::make<f16_t>();
        case scalar_kind_t::e5m2_k: return casts_punned_t::make<e5m2_t>();
        case scalar_kind_t::e4m3_k: return casts_punned_t::make<e4m3_t>();
        case scalar_kind_t::e3m2_k: return casts_punned_t::make<e3m2_t>();
        case scalar_kind_t::e2m3_k: return casts_punned_t::make<e2m3_t>();
        case scalar_kind_t::i8_k: return casts_punned_t::make<i8_t>();
        case scalar_kind_t::u8_k: return casts_punned_t::make<u8_t>();
        case scalar_kind_t::b1x8_k: return casts_punned_t::make<b1x8_t>();
        default: return {};
        }
    }
};

} // namespace usearch
} // namespace unum
