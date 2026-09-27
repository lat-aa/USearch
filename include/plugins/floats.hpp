/**
 *  @file       floats.hpp
 *  @brief      f16/bf16/fp8/fp6 位类型与升降转换。
 */
#pragma once
#include <plugins/kinds.hpp>
namespace unum {
namespace usearch {

/**
 *  @brief Convenience function to upcast a half-precision floating point number to a single-precision one.
 */
inline float f16_to_f32(std::uint16_t u16) noexcept {
#if USEARCH_USE_NUMKONG
    nk_f32_t result;
    nk_f16_to_f32_serial((nk_f16_t const*)&u16, &result);
    return result;
#else
    std::uint32_t sign = (u16 >> 15) & 1;
    std::uint32_t exponent = (u16 >> 10) & 0x1F;
    std::uint32_t mantissa = u16 & 0x03FF;
    fu32_t conv;
    if (exponent == 0) {
        if (mantissa == 0) {
            conv.u = sign << 31;
        } else {
            // Denormal: use FPU normalization trick
            fu32_t temp;
            temp.f = (float)mantissa;
            conv.u = (sign << 31) | (temp.u - 0x0C000000u);
        }
    } else if (exponent == 31) {
        conv.u = (sign << 31) | 0x7F800000u | (mantissa << 13);
    } else {
        conv.u = (sign << 31) | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    return conv.f;
#endif
}

/**
 *  @brief Convenience function to downcast a single-precision floating point number to a half-precision one.
 */
inline std::uint16_t f32_to_f16(float f32) noexcept {
#if USEARCH_USE_NUMKONG
    nk_f16_t result;
    nk_f32_to_f16_serial((nk_f32_t const*)&f32, &result);
    std::uint16_t u16;
    std::memcpy(&u16, &result, sizeof(u16));
    return u16;
#else
    fu32_t conv;
    conv.f = f32;
    std::uint32_t sign = (conv.u >> 31) & 1;
    std::uint32_t exponent = (conv.u >> 23) & 0xFF;
    std::uint32_t mantissa = conv.u & 0x007FFFFFu;
    std::uint16_t result;
    if (exponent == 0) {
        result = (std::uint16_t)(sign << 15);
    } else if (exponent == 255) {
        std::uint16_t payload = (std::uint16_t)(mantissa >> 13);
        if (mantissa != 0 && payload == 0)
            payload = 1;
        result = (std::uint16_t)((sign << 15) | 0x7C00 | payload);
    } else if (exponent <= 102) {
        if (exponent == 102 && mantissa > 0)
            result = (std::uint16_t)((sign << 15) | 0x0001);
        else
            result = (std::uint16_t)(sign << 15);
    } else if (exponent < 113) {
        // Denormal range with RNE rounding
        unsigned shift = 113 - exponent;
        unsigned shift_amount = shift + 13;
        std::uint64_t full_mant = 0x00800000ULL | mantissa;
        std::uint32_t mant = (std::uint32_t)(full_mant >> shift_amount);
        std::uint32_t round_bit = (std::uint32_t)((full_mant >> (shift_amount - 1)) & 1);
        std::uint64_t sticky_bits = full_mant & ((1ULL << (shift_amount - 1)) - 1);
        if (round_bit && (sticky_bits || (mant & 1)))
            mant++;
        result = (std::uint16_t)((sign << 15) | mant);
    } else if (exponent < 143) {
        // Normal range with RNE rounding
        std::uint32_t f16_exp = exponent - 112;
        std::uint32_t f16_mant = mantissa >> 13;
        std::uint32_t round_bit = (mantissa >> 12) & 1;
        std::uint32_t sticky_bits = mantissa & 0xFFF;
        if (round_bit && (sticky_bits || (f16_mant & 1))) {
            f16_mant++;
            if (f16_mant > 0x3FF) {
                f16_mant = 0;
                f16_exp++;
            }
        }
        if (f16_exp > 30)
            result = (std::uint16_t)((sign << 15) | 0x7C00);
        else
            result = (std::uint16_t)((sign << 15) | (f16_exp << 10) | f16_mant);
    } else {
        result = (std::uint16_t)((sign << 15) | 0x7C00);
    }
    return result;
#endif
}

/**
 *  @brief Convenience function to upcast a brain-floating point number to a single-precision one.
 *  https://github.com/ashvardanian/NumKong/blob/7e58e9fee9e096238cf29f7c30774fa3dcd0fe85/include/numkong/cast/serial.h#L226-L244
 */
inline float bf16_to_f32(std::uint16_t u16) noexcept {
#if USEARCH_USE_NUMKONG
    nk_f32_t result;
    nk_bf16_to_f32_serial((nk_bf16_t const*)&u16, &result);
    return result;
#else
    union float_or_unsigned_int_t {
        float f;
        unsigned int i;
    } conv;
    conv.i = u16 << 16; // Zero extends the mantissa
    return conv.f;
#endif
}

/**
 *  @brief Convenience function to downcast a single-precision floating point number to a brain-floating point one.
 *  https://github.com/ashvardanian/NumKong/blob/7e58e9fee9e096238cf29f7c30774fa3dcd0fe85/include/numkong/cast/serial.h#L244-L262
 */
inline std::uint16_t f32_to_bf16(float f32) noexcept {
#if USEARCH_USE_NUMKONG
    nk_bf16_t result;
    nk_f32_to_bf16_serial((nk_f32_t const*)&f32, &result);
    std::uint16_t u16;
    std::memcpy(&u16, &result, sizeof(u16));
    return u16;
#else
    union float_or_unsigned_int_t {
        float f;
        unsigned int i;
    } conv;
    conv.f = f32;
    conv.i >>= 16;
    conv.i &= 0xFFFF;
    return (unsigned short)conv.i;
#endif
}

/**
 *  @brief  Numeric type for the IEEE 754 half-precision floating point.
 *          If hardware support isn't available, falls back to a hardware
 *          agnostic in-software implementation.
 */
class f16_bits_t {
    std::uint16_t uint16_{};

  public:
    inline f16_bits_t() noexcept : uint16_(0) {}
    inline f16_bits_t(f16_bits_t&&) = default;
    inline f16_bits_t& operator=(f16_bits_t&&) = default;
    inline f16_bits_t(f16_bits_t const&) = default;
    inline f16_bits_t& operator=(f16_bits_t const&) = default;

    inline operator float() const noexcept { return f16_to_f32(uint16_); }
    inline explicit operator bool() const noexcept { return f16_to_f32(uint16_) > 0.5f; }

    inline f16_bits_t(int v) noexcept : uint16_(f32_to_f16(static_cast<float>(v))) {}
    inline f16_bits_t(bool v) noexcept : uint16_(f32_to_f16(static_cast<float>(v))) {}
    inline f16_bits_t(float v) noexcept : uint16_(f32_to_f16(v)) {}
    inline f16_bits_t(double v) noexcept : uint16_(f32_to_f16(static_cast<float>(v))) {}

    inline bool operator<(f16_bits_t const& other) const noexcept { return float(*this) < float(other); }

    inline f16_bits_t operator+(f16_bits_t other) const noexcept { return {float(*this) + float(other)}; }
    inline f16_bits_t operator-(f16_bits_t other) const noexcept { return {float(*this) - float(other)}; }
    inline f16_bits_t operator*(f16_bits_t other) const noexcept { return {float(*this) * float(other)}; }
    inline f16_bits_t operator/(f16_bits_t other) const noexcept { return {float(*this) / float(other)}; }
    inline float operator+(float other) const noexcept { return float(*this) + other; }
    inline float operator-(float other) const noexcept { return float(*this) - other; }
    inline float operator*(float other) const noexcept { return float(*this) * other; }
    inline float operator/(float other) const noexcept { return float(*this) / other; }
    inline double operator+(double other) const noexcept { return float(*this) + other; }
    inline double operator-(double other) const noexcept { return float(*this) - other; }
    inline double operator*(double other) const noexcept { return float(*this) * other; }
    inline double operator/(double other) const noexcept { return float(*this) / other; }

    inline f16_bits_t& operator+=(float v) noexcept {
        uint16_ = f32_to_f16(v + f16_to_f32(uint16_));
        return *this;
    }

    inline f16_bits_t& operator-=(float v) noexcept {
        uint16_ = f32_to_f16(v - f16_to_f32(uint16_));
        return *this;
    }

    inline f16_bits_t& operator*=(float v) noexcept {
        uint16_ = f32_to_f16(v * f16_to_f32(uint16_));
        return *this;
    }

    inline f16_bits_t& operator/=(float v) noexcept {
        uint16_ = f32_to_f16(v / f16_to_f32(uint16_));
        return *this;
    }
};

#if USEARCH_USE_OPENMP
#pragma omp declare reduction(+ : unum::usearch::f16_bits_t : omp_out = omp_out + omp_in)                              \
    initializer(omp_priv = unum::usearch::f16_bits_t())
#endif

/**
 *  @brief  Numeric type for brain-floating point half-precision floating point.
 *          If hardware support isn't available, falls back to a hardware
 *          agnostic in-software implementation.
 */
class bf16_bits_t {
    std::uint16_t uint16_{};

  public:
    inline bf16_bits_t() noexcept : uint16_(0) {}
    inline bf16_bits_t(bf16_bits_t&&) = default;
    inline bf16_bits_t& operator=(bf16_bits_t&&) = default;
    inline bf16_bits_t(bf16_bits_t const&) = default;
    inline bf16_bits_t& operator=(bf16_bits_t const&) = default;

    inline operator float() const noexcept { return bf16_to_f32(uint16_); }
    inline explicit operator bool() const noexcept { return bf16_to_f32(uint16_) > 0.5f; }

    inline bf16_bits_t(int v) noexcept : uint16_(f32_to_bf16(static_cast<float>(v))) {}
    inline bf16_bits_t(bool v) noexcept : uint16_(f32_to_bf16(static_cast<float>(v))) {}
    inline bf16_bits_t(float v) noexcept : uint16_(f32_to_bf16(v)) {}
    inline bf16_bits_t(double v) noexcept : uint16_(f32_to_bf16(static_cast<float>(v))) {}

    inline bool operator<(bf16_bits_t const& other) const noexcept { return float(*this) < float(other); }

    inline bf16_bits_t operator+(bf16_bits_t other) const noexcept { return {float(*this) + float(other)}; }
    inline bf16_bits_t operator-(bf16_bits_t other) const noexcept { return {float(*this) - float(other)}; }
    inline bf16_bits_t operator*(bf16_bits_t other) const noexcept { return {float(*this) * float(other)}; }
    inline bf16_bits_t operator/(bf16_bits_t other) const noexcept { return {float(*this) / float(other)}; }
    inline float operator+(float other) const noexcept { return float(*this) + other; }
    inline float operator-(float other) const noexcept { return float(*this) - other; }
    inline float operator*(float other) const noexcept { return float(*this) * other; }
    inline float operator/(float other) const noexcept { return float(*this) / other; }
    inline double operator+(double other) const noexcept { return float(*this) + other; }
    inline double operator-(double other) const noexcept { return float(*this) - other; }
    inline double operator*(double other) const noexcept { return float(*this) * other; }
    inline double operator/(double other) const noexcept { return float(*this) / other; }

    inline bf16_bits_t& operator+=(float v) noexcept {
        uint16_ = f32_to_bf16(v + bf16_to_f32(uint16_));
        return *this;
    }

    inline bf16_bits_t& operator-=(float v) noexcept {
        uint16_ = f32_to_bf16(v - bf16_to_f32(uint16_));
        return *this;
    }

    inline bf16_bits_t& operator*=(float v) noexcept {
        uint16_ = f32_to_bf16(v * bf16_to_f32(uint16_));
        return *this;
    }

    inline bf16_bits_t& operator/=(float v) noexcept {
        uint16_ = f32_to_bf16(v / bf16_to_f32(uint16_));
        return *this;
    }

    inline bf16_bits_t& operator=(float v) noexcept {
        uint16_ = f32_to_bf16(v);
        return *this;
    }
};

#if USEARCH_USE_OPENMP
#pragma omp declare reduction(+ : unum::usearch::bf16_bits_t : omp_out = omp_out + omp_in)                             \
    initializer(omp_priv = unum::usearch::bf16_bits_t())
#endif

/**
 *  @brief Convenience function to upcast an FP8 E5M2 value to single-precision.
 *         E5M2: 1 sign + 5 exponent (bias=15) + 2 mantissa, range +/-57344, supports inf/NaN.
 */
inline float e5m2_to_f32(std::uint8_t u8) noexcept {
#if USEARCH_USE_NUMKONG
    nk_f32_t result;
    nk_e5m2_to_f32_serial((nk_e5m2_t const*)&u8, &result);
    return result;
#else
    // 128-entry LUT for the 7-bit magnitude, sign handled separately.
    static std::uint32_t const lut[128] = {
        0x00000000, 0x37800000, 0x38000000, 0x38400000, // exp=0  sub
        0x38800000, 0x38A00000, 0x38C00000, 0x38E00000, // exp=1
        0x39000000, 0x39200000, 0x39400000, 0x39600000, // exp=2
        0x39800000, 0x39A00000, 0x39C00000, 0x39E00000, // exp=3
        0x3A000000, 0x3A200000, 0x3A400000, 0x3A600000, // exp=4
        0x3A800000, 0x3AA00000, 0x3AC00000, 0x3AE00000, // exp=5
        0x3B000000, 0x3B200000, 0x3B400000, 0x3B600000, // exp=6
        0x3B800000, 0x3BA00000, 0x3BC00000, 0x3BE00000, // exp=7
        0x3C000000, 0x3C200000, 0x3C400000, 0x3C600000, // exp=8
        0x3C800000, 0x3CA00000, 0x3CC00000, 0x3CE00000, // exp=9
        0x3D000000, 0x3D200000, 0x3D400000, 0x3D600000, // exp=10
        0x3D800000, 0x3DA00000, 0x3DC00000, 0x3DE00000, // exp=11
        0x3E000000, 0x3E200000, 0x3E400000, 0x3E600000, // exp=12
        0x3E800000, 0x3EA00000, 0x3EC00000, 0x3EE00000, // exp=13
        0x3F000000, 0x3F200000, 0x3F400000, 0x3F600000, // exp=14
        0x3F800000, 0x3FA00000, 0x3FC00000, 0x3FE00000, // exp=15
        0x40000000, 0x40200000, 0x40400000, 0x40600000, // exp=16
        0x40800000, 0x40A00000, 0x40C00000, 0x40E00000, // exp=17
        0x41000000, 0x41200000, 0x41400000, 0x41600000, // exp=18
        0x41800000, 0x41A00000, 0x41C00000, 0x41E00000, // exp=19
        0x42000000, 0x42200000, 0x42400000, 0x42600000, // exp=20
        0x42800000, 0x42A00000, 0x42C00000, 0x42E00000, // exp=21
        0x43000000, 0x43200000, 0x43400000, 0x43600000, // exp=22
        0x43800000, 0x43A00000, 0x43C00000, 0x43E00000, // exp=23
        0x44000000, 0x44200000, 0x44400000, 0x44600000, // exp=24
        0x44800000, 0x44A00000, 0x44C00000, 0x44E00000, // exp=25
        0x45000000, 0x45200000, 0x45400000, 0x45600000, // exp=26
        0x45800000, 0x45A00000, 0x45C00000, 0x45E00000, // exp=27
        0x46000000, 0x46200000, 0x46400000, 0x46600000, // exp=28
        0x46800000, 0x46A00000, 0x46C00000, 0x46E00000, // exp=29
        0x47000000, 0x47200000, 0x47400000, 0x47600000, // exp=30
        0x7F800000, 0x7FC00000, 0x7FC00000, 0x7FC00000, // inf, nan
    };
    std::uint32_t sign = (std::uint32_t)(u8 & 0x80) << 24;
    fu32_t conv;
    conv.u = sign | lut[u8 & 0x7F];
    return conv.f;
#endif
}

/**
 *  @brief Convenience function to downcast a single-precision value to FP8 E5M2.
 *         Uses RNE rounding. Overflow → inf, NaN → NaN, subnormals handled.
 */
inline std::uint8_t f32_to_e5m2(float f32) noexcept {
#if USEARCH_USE_NUMKONG
    nk_e5m2_t result;
    nk_f32_to_e5m2_serial((nk_f32_t const*)&f32, &result);
    return result;
#else
    fu32_t conv;
    conv.f = f32;
    std::uint32_t sign_bit = conv.u >> 31;
    std::uint32_t abs_bits = conv.u & 0x7FFFFFFFu;
    std::uint8_t sign = (std::uint8_t)(sign_bit << 7);

    // NaN or inf
    if (abs_bits >= 0x7F800000u) {
        std::uint8_t mant = (abs_bits > 0x7F800000u) ? 0x01u : 0x00u;
        return (std::uint8_t)(sign | 0x7Cu | mant);
    }
    if (abs_bits == 0)
        return sign;

    float abs_x = sign_bit ? -f32 : f32;

    // Subnormal range: |x| < 2^-14
    if (abs_x < (1.0f / 16384.0f)) {
        float scaled = abs_x * 65536.0f;
        int mant = (int)scaled;
        float frac = scaled - (float)mant;
        if (frac > 0.5f || (frac == 0.5f && (mant & 1)))
            ++mant;
        if (mant > 3)
            return (std::uint8_t)(sign | 0x04u);
        return (std::uint8_t)(sign | (std::uint8_t)mant);
    }

    int exp = (int)((abs_bits >> 23) & 0xFFu) - 127;
    std::uint32_t mantissa = abs_bits & 0x7FFFFFu;
    std::uint32_t significand = (1u << 23) | mantissa;
    int shift = 23 - 2;
    std::uint32_t remainder_mask = (1u << shift) - 1;
    std::uint32_t remainder = significand & remainder_mask;
    std::uint32_t halfway = 1u << (shift - 1);
    std::uint32_t significand_rounded = significand >> shift;
    if (remainder > halfway || (remainder == halfway && (significand_rounded & 1)))
        ++significand_rounded;
    if (significand_rounded == (1u << 3)) {
        significand_rounded >>= 1;
        ++exp;
    }
    if (exp > 15)
        return (std::uint8_t)(sign | 0x7Cu); // overflow → inf
    if (exp < -14) {
        float scaled = abs_x * 65536.0f;
        int mant = (int)scaled;
        float frac = scaled - (float)mant;
        if (frac > 0.5f || (frac == 0.5f && (mant & 1)))
            ++mant;
        if (mant > 3)
            return (std::uint8_t)(sign | 0x04u);
        return (std::uint8_t)(sign | (std::uint8_t)mant);
    }

    std::uint8_t exp_field = (std::uint8_t)(exp + 15);
    std::uint8_t mant_field = (std::uint8_t)(significand_rounded & 0x03u);
    return (std::uint8_t)(sign | (exp_field << 2) | mant_field);
#endif
}

/**
 *  @brief Convenience function to upcast an FP8 E4M3 value to single-precision.
 *         E4M3: 1 sign + 4 exponent (bias=7) + 3 mantissa, range +/-448, no inf.
 */
inline float e4m3_to_f32(std::uint8_t u8) noexcept {
#if USEARCH_USE_NUMKONG
    nk_f32_t result;
    nk_e4m3_to_f32_serial((nk_e4m3_t const*)&u8, &result);
    return result;
#else
    static std::uint32_t const lut[128] = {
        0x00000000, 0x3B000000, 0x3B800000, 0x3BC00000, 0x3C000000, 0x3C200000, 0x3C400000, 0x3C600000, // exp=0 sub
        0x3C800000, 0x3C900000, 0x3CA00000, 0x3CB00000, 0x3CC00000, 0x3CD00000, 0x3CE00000, 0x3CF00000, // exp=1
        0x3D000000, 0x3D100000, 0x3D200000, 0x3D300000, 0x3D400000, 0x3D500000, 0x3D600000, 0x3D700000, // exp=2
        0x3D800000, 0x3D900000, 0x3DA00000, 0x3DB00000, 0x3DC00000, 0x3DD00000, 0x3DE00000, 0x3DF00000, // exp=3
        0x3E000000, 0x3E100000, 0x3E200000, 0x3E300000, 0x3E400000, 0x3E500000, 0x3E600000, 0x3E700000, // exp=4
        0x3E800000, 0x3E900000, 0x3EA00000, 0x3EB00000, 0x3EC00000, 0x3ED00000, 0x3EE00000, 0x3EF00000, // exp=5
        0x3F000000, 0x3F100000, 0x3F200000, 0x3F300000, 0x3F400000, 0x3F500000, 0x3F600000, 0x3F700000, // exp=6
        0x3F800000, 0x3F900000, 0x3FA00000, 0x3FB00000, 0x3FC00000, 0x3FD00000, 0x3FE00000, 0x3FF00000, // exp=7
        0x40000000, 0x40100000, 0x40200000, 0x40300000, 0x40400000, 0x40500000, 0x40600000, 0x40700000, // exp=8
        0x40800000, 0x40900000, 0x40A00000, 0x40B00000, 0x40C00000, 0x40D00000, 0x40E00000, 0x40F00000, // exp=9
        0x41000000, 0x41100000, 0x41200000, 0x41300000, 0x41400000, 0x41500000, 0x41600000, 0x41700000, // exp=10
        0x41800000, 0x41900000, 0x41A00000, 0x41B00000, 0x41C00000, 0x41D00000, 0x41E00000, 0x41F00000, // exp=11
        0x42000000, 0x42100000, 0x42200000, 0x42300000, 0x42400000, 0x42500000, 0x42600000, 0x42700000, // exp=12
        0x42800000, 0x42900000, 0x42A00000, 0x42B00000, 0x42C00000, 0x42D00000, 0x42E00000, 0x42F00000, // exp=13
        0x43000000, 0x43100000, 0x43200000, 0x43300000, 0x43400000, 0x43500000, 0x43600000, 0x43700000, // exp=14
        0x43800000, 0x43900000, 0x43A00000, 0x43B00000, 0x43C00000, 0x43D00000, 0x43E00000, 0x7FC00000, // exp=15
    };
    std::uint32_t sign = (std::uint32_t)(u8 & 0x80) << 24;
    fu32_t conv;
    conv.u = sign | lut[u8 & 0x7F];
    return conv.f;
#endif
}

/**
 *  @brief Convenience function to downcast a single-precision value to FP8 E4M3.
 *         Uses RNE rounding. Overflow saturates to +/-448 (no inf in E4M3FN).
 */
inline std::uint8_t f32_to_e4m3(float f32) noexcept {
#if USEARCH_USE_NUMKONG
    nk_e4m3_t result;
    nk_f32_to_e4m3_serial((nk_f32_t const*)&f32, &result);
    return result;
#else
    fu32_t conv;
    conv.f = f32;
    std::uint32_t sign_bit = conv.u >> 31;
    std::uint32_t abs_bits = conv.u & 0x7FFFFFFFu;
    std::uint8_t sign = (std::uint8_t)(sign_bit << 7);

    // NaN → E4M3FN NaN
    if (abs_bits > 0x7F800000u)
        return (std::uint8_t)(sign | 0x7Fu);
    // Inf → saturate to max (448)
    if (abs_bits == 0x7F800000u)
        return (std::uint8_t)(sign | 0x7Eu);
    if (abs_bits == 0)
        return sign;

    float abs_x = sign_bit ? -f32 : f32;

    // Subnormal range: |x| < 2^-6
    if (abs_x < (1.0f / 64.0f)) {
        float scaled = abs_x * 512.0f;
        int mant = (int)scaled;
        float frac = scaled - (float)mant;
        if (frac > 0.5f || (frac == 0.5f && (mant & 1)))
            ++mant;
        if (mant > 7)
            return (std::uint8_t)(sign | 0x08u);
        return (std::uint8_t)(sign | (std::uint8_t)mant);
    }

    int exp = (int)((abs_bits >> 23) & 0xFFu) - 127;
    std::uint32_t mantissa = abs_bits & 0x7FFFFFu;
    std::uint32_t significand = (1u << 23) | mantissa;
    int shift = 23 - 3;
    std::uint32_t remainder_mask = (1u << shift) - 1;
    std::uint32_t remainder = significand & remainder_mask;
    std::uint32_t halfway = 1u << (shift - 1);
    std::uint32_t significand_rounded = significand >> shift;
    if (remainder > halfway || (remainder == halfway && (significand_rounded & 1)))
        ++significand_rounded;
    if (significand_rounded == (1u << 4)) {
        significand_rounded >>= 1;
        ++exp;
    }
    // Overflow → saturate to max (0x7E = 448)
    if (exp > 8)
        return (std::uint8_t)(sign | 0x7Eu);
    if (exp < -6) {
        float scaled = abs_x * 512.0f;
        int mant = (int)scaled;
        float frac = scaled - (float)mant;
        if (frac > 0.5f || (frac == 0.5f && (mant & 1)))
            ++mant;
        if (mant > 7)
            return (std::uint8_t)(sign | 0x08u);
        return (std::uint8_t)(sign | (std::uint8_t)mant);
    }

    std::uint8_t exp_field = (std::uint8_t)(exp + 7);
    std::uint8_t mant_field = (std::uint8_t)(significand_rounded & 0x07u);
    // Clamp to avoid NaN encoding (0x7F)
    if (exp_field == 15 && mant_field > 6)
        mant_field = 6;
    return (std::uint8_t)(sign | (exp_field << 3) | mant_field);
#endif
}

/**
 *  @brief  Numeric type for FP8 E5M2 (IEEE 754-like) floating point.
 *          1 sign + 5 exponent + 2 mantissa bits, range +/-57344.
 */
class e5m2_bits_t {
    std::uint8_t uint8_{};

  public:
    inline e5m2_bits_t() noexcept : uint8_(0) {}
    inline e5m2_bits_t(e5m2_bits_t&&) = default;
    inline e5m2_bits_t& operator=(e5m2_bits_t&&) = default;
    inline e5m2_bits_t(e5m2_bits_t const&) = default;
    inline e5m2_bits_t& operator=(e5m2_bits_t const&) = default;

    inline operator float() const noexcept { return e5m2_to_f32(uint8_); }
    inline explicit operator bool() const noexcept { return e5m2_to_f32(uint8_) > 0.5f; }

    inline e5m2_bits_t(int v) noexcept : uint8_(f32_to_e5m2(static_cast<float>(v))) {}
    inline e5m2_bits_t(bool v) noexcept : uint8_(f32_to_e5m2(static_cast<float>(v))) {}
    inline e5m2_bits_t(float v) noexcept : uint8_(f32_to_e5m2(v)) {}
    inline e5m2_bits_t(double v) noexcept : uint8_(f32_to_e5m2(static_cast<float>(v))) {}

    inline bool operator<(e5m2_bits_t const& other) const noexcept { return float(*this) < float(other); }

    inline e5m2_bits_t operator+(e5m2_bits_t other) const noexcept { return {float(*this) + float(other)}; }
    inline e5m2_bits_t operator-(e5m2_bits_t other) const noexcept { return {float(*this) - float(other)}; }
    inline e5m2_bits_t operator*(e5m2_bits_t other) const noexcept { return {float(*this) * float(other)}; }
    inline e5m2_bits_t operator/(e5m2_bits_t other) const noexcept { return {float(*this) / float(other)}; }
    inline float operator+(float other) const noexcept { return float(*this) + other; }
    inline float operator-(float other) const noexcept { return float(*this) - other; }
    inline float operator*(float other) const noexcept { return float(*this) * other; }
    inline float operator/(float other) const noexcept { return float(*this) / other; }
    inline double operator+(double other) const noexcept { return float(*this) + other; }
    inline double operator-(double other) const noexcept { return float(*this) - other; }
    inline double operator*(double other) const noexcept { return float(*this) * other; }
    inline double operator/(double other) const noexcept { return float(*this) / other; }

    inline e5m2_bits_t& operator+=(float v) noexcept {
        uint8_ = f32_to_e5m2(v + e5m2_to_f32(uint8_));
        return *this;
    }
    inline e5m2_bits_t& operator-=(float v) noexcept {
        uint8_ = f32_to_e5m2(v - e5m2_to_f32(uint8_));
        return *this;
    }
    inline e5m2_bits_t& operator*=(float v) noexcept {
        uint8_ = f32_to_e5m2(v * e5m2_to_f32(uint8_));
        return *this;
    }
    inline e5m2_bits_t& operator/=(float v) noexcept {
        uint8_ = f32_to_e5m2(v / e5m2_to_f32(uint8_));
        return *this;
    }
    inline e5m2_bits_t& operator=(float v) noexcept {
        uint8_ = f32_to_e5m2(v);
        return *this;
    }
};

/**
 *  @brief  Numeric type for FP8 E4M3 (OCP) floating point.
 *          1 sign + 4 exponent + 3 mantissa bits, range +/-448.
 */
class e4m3_bits_t {
    std::uint8_t uint8_{};

  public:
    inline e4m3_bits_t() noexcept : uint8_(0) {}
    inline e4m3_bits_t(e4m3_bits_t&&) = default;
    inline e4m3_bits_t& operator=(e4m3_bits_t&&) = default;
    inline e4m3_bits_t(e4m3_bits_t const&) = default;
    inline e4m3_bits_t& operator=(e4m3_bits_t const&) = default;

    inline operator float() const noexcept { return e4m3_to_f32(uint8_); }
    inline explicit operator bool() const noexcept { return e4m3_to_f32(uint8_) > 0.5f; }

    inline e4m3_bits_t(int v) noexcept : uint8_(f32_to_e4m3(static_cast<float>(v))) {}
    inline e4m3_bits_t(bool v) noexcept : uint8_(f32_to_e4m3(static_cast<float>(v))) {}
    inline e4m3_bits_t(float v) noexcept : uint8_(f32_to_e4m3(v)) {}
    inline e4m3_bits_t(double v) noexcept : uint8_(f32_to_e4m3(static_cast<float>(v))) {}

    inline bool operator<(e4m3_bits_t const& other) const noexcept { return float(*this) < float(other); }

    inline e4m3_bits_t operator+(e4m3_bits_t other) const noexcept { return {float(*this) + float(other)}; }
    inline e4m3_bits_t operator-(e4m3_bits_t other) const noexcept { return {float(*this) - float(other)}; }
    inline e4m3_bits_t operator*(e4m3_bits_t other) const noexcept { return {float(*this) * float(other)}; }
    inline e4m3_bits_t operator/(e4m3_bits_t other) const noexcept { return {float(*this) / float(other)}; }
    inline float operator+(float other) const noexcept { return float(*this) + other; }
    inline float operator-(float other) const noexcept { return float(*this) - other; }
    inline float operator*(float other) const noexcept { return float(*this) * other; }
    inline float operator/(float other) const noexcept { return float(*this) / other; }
    inline double operator+(double other) const noexcept { return float(*this) + other; }
    inline double operator-(double other) const noexcept { return float(*this) - other; }
    inline double operator*(double other) const noexcept { return float(*this) * other; }
    inline double operator/(double other) const noexcept { return float(*this) / other; }

    inline e4m3_bits_t& operator+=(float v) noexcept {
        uint8_ = f32_to_e4m3(v + e4m3_to_f32(uint8_));
        return *this;
    }
    inline e4m3_bits_t& operator-=(float v) noexcept {
        uint8_ = f32_to_e4m3(v - e4m3_to_f32(uint8_));
        return *this;
    }
    inline e4m3_bits_t& operator*=(float v) noexcept {
        uint8_ = f32_to_e4m3(v * e4m3_to_f32(uint8_));
        return *this;
    }
    inline e4m3_bits_t& operator/=(float v) noexcept {
        uint8_ = f32_to_e4m3(v / e4m3_to_f32(uint8_));
        return *this;
    }
    inline e4m3_bits_t& operator=(float v) noexcept {
        uint8_ = f32_to_e4m3(v);
        return *this;
    }
};

/**
 *  @brief Convenience function to upcast an FP6 E2M3 value to single-precision.
 *         E2M3: 1 sign + 2 exponent (bias=1) + 3 mantissa, stored as 0b00SEEMMM, range +/-7.5.
 */
inline float e2m3_to_f32(std::uint8_t u8) noexcept {
#if USEARCH_USE_NUMKONG
    nk_f32_t result;
    nk_e2m3_to_f32_serial((nk_e2m3_t const*)&u8, &result);
    return result;
#else
    static std::uint32_t const lut[64] = {
        0x00000000, 0x3E000000, 0x3E800000, 0x3EC00000, 0x3F000000, 0x3F200000, 0x3F400000, 0x3F600000, // positive
        0x3F800000, 0x3F900000, 0x3FA00000, 0x3FB00000, 0x3FC00000, 0x3FD00000, 0x3FE00000, 0x3FF00000, // positive
        0x40000000, 0x40100000, 0x40200000, 0x40300000, 0x40400000, 0x40500000, 0x40600000, 0x40700000, // positive
        0x40800000, 0x40900000, 0x40A00000, 0x40B00000, 0x40C00000, 0x40D00000, 0x40E00000, 0x40F00000, // positive
        0x80000000, 0xBE000000, 0xBE800000, 0xBEC00000, 0xBF000000, 0xBF200000, 0xBF400000, 0xBF600000, // negative
        0xBF800000, 0xBF900000, 0xBFA00000, 0xBFB00000, 0xBFC00000, 0xBFD00000, 0xBFE00000, 0xBFF00000, // negative
        0xC0000000, 0xC0100000, 0xC0200000, 0xC0300000, 0xC0400000, 0xC0500000, 0xC0600000, 0xC0700000, // negative
        0xC0800000, 0xC0900000, 0xC0A00000, 0xC0B00000, 0xC0C00000, 0xC0D00000, 0xC0E00000, 0xC0F00000, // negative
    };
    fu32_t conv;
    conv.u = lut[u8 & 0x3F];
    return conv.f;
#endif
}

/**
 *  @brief Convenience function to downcast a single-precision value to FP6 E2M3.
 */
inline std::uint8_t f32_to_e2m3(float f32) noexcept {
#if USEARCH_USE_NUMKONG
    nk_e2m3_t result;
    nk_f32_to_e2m3_serial((nk_f32_t const*)&f32, &result);
    return result;
#else
    fu32_t conv;
    conv.f = f32;
    std::uint32_t sign_bit = conv.u >> 31;
    std::uint32_t abs_bits = conv.u & 0x7FFFFFFFu;
    std::uint8_t sign = (std::uint8_t)(sign_bit << 5);
    if (abs_bits == 0)
        return sign;
    float abs_x = sign_bit ? -f32 : f32;
    // E2M3: bias=1, 2 exp bits, 3 mant bits, max normal = 7.5, min subnormal = 0.125
    if (abs_x >= 7.5f)
        return (std::uint8_t)(sign | 0x1Fu); // saturate to max
    if (abs_x < 0.0625f)
        return sign; // underflow to zero
    // Subnormal range: abs_x < 1.0 (min normal = 2^(1-1) = 1.0)
    if (abs_x < 1.0f) {
        // Subnormals: value = mant * 2^(-3) = mant/8, so mant = round(abs_x * 8)
        std::uint32_t mant = (std::uint32_t)(abs_x * 8.0f + 0.5f);
        if (mant > 7)
            mant = 7;
        if (mant == 0)
            return sign;
        return (std::uint8_t)(sign | mant);
    }
    // Normal range: value = (1 + mant/8) * 2^(exp-1)
    // Find exponent: exp-1 = floor(log2(abs_x)), so exp = floor(log2(abs_x)) + 1
    int exp_val;
    float frac = std::frexp(abs_x, &exp_val); // abs_x = frac * 2^exp_val, frac in [0.5, 1)
    // frexp returns frac in [0.5, 1), but we want significand in [1, 2)
    // so significand = frac * 2, and true_exp = exp_val - 1
    float significand = frac * 2.0f; // [1.0, 2.0)
    int biased_exp = exp_val;        // exp_val - 1 + bias(1) = exp_val
    if (biased_exp < 1) {
        // Fell into subnormal, handled above
        std::uint32_t mant = (std::uint32_t)(abs_x * 8.0f + 0.5f);
        if (mant > 7)
            mant = 7;
        return (std::uint8_t)(sign | mant);
    }
    if (biased_exp > 3)
        biased_exp = 3; // clamp to max exp
    // Round mantissa: 3 mant bits, significand in [1, 2)
    std::uint32_t mant = (std::uint32_t)((significand - 1.0f) * 8.0f + 0.5f);
    if (mant > 7) {
        mant = 0;
        biased_exp++;
    }
    if (biased_exp > 3)
        return (std::uint8_t)(sign | 0x1Fu); // overflow
    return (std::uint8_t)(sign | (biased_exp << 3) | mant);
#endif
}

/**
 *  @brief Convenience function to upcast an FP6 E3M2 value to single-precision.
 *         E3M2: 1 sign + 3 exponent (bias=3) + 2 mantissa, stored as 0b00SEEEMM, range +/-28.
 */
inline float e3m2_to_f32(std::uint8_t u8) noexcept {
#if USEARCH_USE_NUMKONG
    nk_f32_t result;
    nk_e3m2_to_f32_serial((nk_e3m2_t const*)&u8, &result);
    return result;
#else
    static std::uint32_t const lut[64] = {
        0x00000000, 0x3D800000, 0x3E000000, 0x3E400000, 0x3E800000, 0x3EA00000, 0x3EC00000, 0x3EE00000, // positive
        0x3F000000, 0x3F200000, 0x3F400000, 0x3F600000, 0x3F800000, 0x3FA00000, 0x3FC00000, 0x3FE00000, // positive
        0x40000000, 0x40200000, 0x40400000, 0x40600000, 0x40800000, 0x40A00000, 0x40C00000, 0x40E00000, // positive
        0x41000000, 0x41200000, 0x41400000, 0x41600000, 0x41800000, 0x41A00000, 0x41C00000, 0x41E00000, // positive
        0x80000000, 0xBD800000, 0xBE000000, 0xBE400000, 0xBE800000, 0xBEA00000, 0xBEC00000, 0xBEE00000, // negative
        0xBF000000, 0xBF200000, 0xBF400000, 0xBF600000, 0xBF800000, 0xBFA00000, 0xBFC00000, 0xBFE00000, // negative
        0xC0000000, 0xC0200000, 0xC0400000, 0xC0600000, 0xC0800000, 0xC0A00000, 0xC0C00000, 0xC0E00000, // negative
        0xC1000000, 0xC1200000, 0xC1400000, 0xC1600000, 0xC1800000, 0xC1A00000, 0xC1C00000, 0xC1E00000, // negative
    };
    fu32_t conv;
    conv.u = lut[u8 & 0x3F];
    return conv.f;
#endif
}

/**
 *  @brief Convenience function to downcast a single-precision value to FP6 E3M2.
 */
inline std::uint8_t f32_to_e3m2(float f32) noexcept {
#if USEARCH_USE_NUMKONG
    nk_e3m2_t result;
    nk_f32_to_e3m2_serial((nk_f32_t const*)&f32, &result);
    return result;
#else
    fu32_t conv;
    conv.f = f32;
    std::uint32_t sign_bit = conv.u >> 31;
    std::uint32_t abs_bits = conv.u & 0x7FFFFFFFu;
    std::uint8_t sign = (std::uint8_t)(sign_bit << 5);
    if (abs_bits == 0)
        return sign;
    float abs_x = sign_bit ? -f32 : f32;
    // E3M2: bias=3, 3 exp bits, 2 mant bits, max normal = 28.0, min subnormal = 0.0625
    if (abs_x >= 28.0f)
        return (std::uint8_t)(sign | 0x1Fu); // saturate to max
    if (abs_x < 0.03125f)
        return sign; // underflow to zero
    // Subnormal range: abs_x < 0.25 (min normal = 2^(1-3) = 0.25)
    if (abs_x < 0.25f) {
        // Subnormals: value = mant * 2^(-4) = mant/16, so mant = round(abs_x * 16)
        std::uint32_t mant = (std::uint32_t)(abs_x * 16.0f + 0.5f);
        if (mant > 3)
            mant = 3;
        if (mant == 0)
            return sign;
        return (std::uint8_t)(sign | mant);
    }
    // Normal range: value = (1 + mant/4) * 2^(exp-3)
    int exp_val;
    float frac = std::frexp(abs_x, &exp_val);
    float significand = frac * 2.0f;
    int biased_exp = exp_val - 1 + 3; // true_exp = exp_val - 1, biased = true_exp + 3
    if (biased_exp < 1) {
        std::uint32_t mant = (std::uint32_t)(abs_x * 16.0f + 0.5f);
        if (mant > 3)
            mant = 3;
        return (std::uint8_t)(sign | mant);
    }
    if (biased_exp > 7)
        biased_exp = 7;
    std::uint32_t mant = (std::uint32_t)((significand - 1.0f) * 4.0f + 0.5f);
    if (mant > 3) {
        mant = 0;
        biased_exp++;
    }
    if (biased_exp > 7)
        return (std::uint8_t)(sign | 0x1Fu);
    return (std::uint8_t)(sign | (biased_exp << 2) | mant);
#endif
}

/**
 *  @brief  Numeric type for FP6 E2M3 floating point.
 *          1 sign + 2 exponent + 3 mantissa bits, stored as 0b00SEEMMM, range +/-7.5.
 */
class e2m3_bits_t {
    std::uint8_t uint8_{};

  public:
    inline e2m3_bits_t() noexcept : uint8_(0) {}
    inline e2m3_bits_t(e2m3_bits_t&&) = default;
    inline e2m3_bits_t& operator=(e2m3_bits_t&&) = default;
    inline e2m3_bits_t(e2m3_bits_t const&) = default;
    inline e2m3_bits_t& operator=(e2m3_bits_t const&) = default;

    inline operator float() const noexcept { return e2m3_to_f32(uint8_); }
    inline explicit operator bool() const noexcept { return e2m3_to_f32(uint8_) > 0.5f; }

    inline e2m3_bits_t(int v) noexcept : uint8_(f32_to_e2m3(static_cast<float>(v))) {}
    inline e2m3_bits_t(bool v) noexcept : uint8_(f32_to_e2m3(static_cast<float>(v))) {}
    inline e2m3_bits_t(float v) noexcept : uint8_(f32_to_e2m3(v)) {}
    inline e2m3_bits_t(double v) noexcept : uint8_(f32_to_e2m3(static_cast<float>(v))) {}

    inline bool operator<(e2m3_bits_t const& other) const noexcept { return float(*this) < float(other); }

    inline e2m3_bits_t operator+(e2m3_bits_t other) const noexcept { return {float(*this) + float(other)}; }
    inline e2m3_bits_t operator-(e2m3_bits_t other) const noexcept { return {float(*this) - float(other)}; }
    inline e2m3_bits_t operator*(e2m3_bits_t other) const noexcept { return {float(*this) * float(other)}; }
    inline e2m3_bits_t operator/(e2m3_bits_t other) const noexcept { return {float(*this) / float(other)}; }
    inline float operator+(float other) const noexcept { return float(*this) + other; }
    inline float operator-(float other) const noexcept { return float(*this) - other; }
    inline float operator*(float other) const noexcept { return float(*this) * other; }
    inline float operator/(float other) const noexcept { return float(*this) / other; }
    inline double operator+(double other) const noexcept { return float(*this) + other; }
    inline double operator-(double other) const noexcept { return float(*this) - other; }
    inline double operator*(double other) const noexcept { return float(*this) * other; }
    inline double operator/(double other) const noexcept { return float(*this) / other; }

    inline e2m3_bits_t& operator+=(float v) noexcept {
        uint8_ = f32_to_e2m3(v + e2m3_to_f32(uint8_));
        return *this;
    }
    inline e2m3_bits_t& operator-=(float v) noexcept {
        uint8_ = f32_to_e2m3(v - e2m3_to_f32(uint8_));
        return *this;
    }
    inline e2m3_bits_t& operator*=(float v) noexcept {
        uint8_ = f32_to_e2m3(v * e2m3_to_f32(uint8_));
        return *this;
    }
    inline e2m3_bits_t& operator/=(float v) noexcept {
        uint8_ = f32_to_e2m3(v / e2m3_to_f32(uint8_));
        return *this;
    }
    inline e2m3_bits_t& operator=(float v) noexcept {
        uint8_ = f32_to_e2m3(v);
        return *this;
    }
};

/**
 *  @brief  Numeric type for FP6 E3M2 floating point.
 *          1 sign + 3 exponent + 2 mantissa bits, stored as 0b00SEEEMM, range +/-28.
 */
class e3m2_bits_t {
    std::uint8_t uint8_{};

  public:
    inline e3m2_bits_t() noexcept : uint8_(0) {}
    inline e3m2_bits_t(e3m2_bits_t&&) = default;
    inline e3m2_bits_t& operator=(e3m2_bits_t&&) = default;
    inline e3m2_bits_t(e3m2_bits_t const&) = default;
    inline e3m2_bits_t& operator=(e3m2_bits_t const&) = default;

    inline operator float() const noexcept { return e3m2_to_f32(uint8_); }
    inline explicit operator bool() const noexcept { return e3m2_to_f32(uint8_) > 0.5f; }

    inline e3m2_bits_t(int v) noexcept : uint8_(f32_to_e3m2(static_cast<float>(v))) {}
    inline e3m2_bits_t(bool v) noexcept : uint8_(f32_to_e3m2(static_cast<float>(v))) {}
    inline e3m2_bits_t(float v) noexcept : uint8_(f32_to_e3m2(v)) {}
    inline e3m2_bits_t(double v) noexcept : uint8_(f32_to_e3m2(static_cast<float>(v))) {}

    inline bool operator<(e3m2_bits_t const& other) const noexcept { return float(*this) < float(other); }

    inline e3m2_bits_t operator+(e3m2_bits_t other) const noexcept { return {float(*this) + float(other)}; }
    inline e3m2_bits_t operator-(e3m2_bits_t other) const noexcept { return {float(*this) - float(other)}; }
    inline e3m2_bits_t operator*(e3m2_bits_t other) const noexcept { return {float(*this) * float(other)}; }
    inline e3m2_bits_t operator/(e3m2_bits_t other) const noexcept { return {float(*this) / float(other)}; }
    inline float operator+(float other) const noexcept { return float(*this) + other; }
    inline float operator-(float other) const noexcept { return float(*this) - other; }
    inline float operator*(float other) const noexcept { return float(*this) * other; }
    inline float operator/(float other) const noexcept { return float(*this) / other; }
    inline double operator+(double other) const noexcept { return float(*this) + other; }
    inline double operator-(double other) const noexcept { return float(*this) - other; }
    inline double operator*(double other) const noexcept { return float(*this) * other; }
    inline double operator/(double other) const noexcept { return float(*this) / other; }

    inline e3m2_bits_t& operator+=(float v) noexcept {
        uint8_ = f32_to_e3m2(v + e3m2_to_f32(uint8_));
        return *this;
    }
    inline e3m2_bits_t& operator-=(float v) noexcept {
        uint8_ = f32_to_e3m2(v - e3m2_to_f32(uint8_));
        return *this;
    }
    inline e3m2_bits_t& operator*=(float v) noexcept {
        uint8_ = f32_to_e3m2(v * e3m2_to_f32(uint8_));
        return *this;
    }
    inline e3m2_bits_t& operator/=(float v) noexcept {
        uint8_ = f32_to_e3m2(v / e3m2_to_f32(uint8_));
        return *this;
    }
    inline e3m2_bits_t& operator=(float v) noexcept {
        uint8_ = f32_to_e3m2(v);
        return *this;
    }
};

#if USEARCH_USE_OPENMP
#pragma omp declare reduction(+ : unum::usearch::e5m2_bits_t : omp_out = omp_out + omp_in)                             \
    initializer(omp_priv = unum::usearch::e5m2_bits_t())
#pragma omp declare reduction(+ : unum::usearch::e4m3_bits_t : omp_out = omp_out + omp_in)                             \
    initializer(omp_priv = unum::usearch::e4m3_bits_t())
#pragma omp declare reduction(+ : unum::usearch::e2m3_bits_t : omp_out = omp_out + omp_in)                             \
    initializer(omp_priv = unum::usearch::e2m3_bits_t())
#pragma omp declare reduction(+ : unum::usearch::e3m2_bits_t : omp_out = omp_out + omp_in)                             \
    initializer(omp_priv = unum::usearch::e3m2_bits_t())
#endif

} // namespace usearch
} // namespace unum
