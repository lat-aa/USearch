/**
 *  @file       isa.hpp
 *  @brief      NumKong capability / ISA 名称表。
 */
#pragma once
#include <plugins/metrics.hpp>
namespace unum {
namespace usearch {

/**
 *  @brief  The signature of the user-defined function.
 *          Can be just two array pointers, precompiled for a specific array length,
 *          or include one or two array sizes as 64-bit unsigned integers.
 */
enum class metric_punned_signature_t {
    array_array_k = 0,
    array_array_size_k,
    array_array_state_k,
};

#if USEARCH_USE_NUMKONG

/**
 *  @brief  Returns the result of `nk_capabilities()`, cached in a function-local static.
 *          Every call-site that needs the current CPU's capability mask should use this
 *          instead of caching the value in its own `static` local.
 */
inline nk_capability_t nk_cached_capabilities() noexcept {
    static nk_capability_t caps = [] {
        nk_capability_t c = nk_capabilities();
        nk_configure_thread(c);
        return c;
    }();
    return caps;
}

/**
 *  @brief  Converts a USearch `scalar_kind_t` to the corresponding NumKong `nk_dtype_t`.
 *  @return The matching dtype, or `(nk_dtype_t)0` when there is no NumKong equivalent.
 */
inline nk_dtype_t scalar_kind_to_nk_dtype(scalar_kind_t sk) noexcept {
    switch (sk) {
    case scalar_kind_t::f64_k: return (nk_dtype_t)nk_f64_k;
    case scalar_kind_t::f32_k: return (nk_dtype_t)nk_f32_k;
    case scalar_kind_t::bf16_k: return (nk_dtype_t)nk_bf16_k;
    case scalar_kind_t::f16_k: return (nk_dtype_t)nk_f16_k;
    case scalar_kind_t::e5m2_k: return (nk_dtype_t)nk_e5m2_k;
    case scalar_kind_t::e4m3_k: return (nk_dtype_t)nk_e4m3_k;
    case scalar_kind_t::e3m2_k: return (nk_dtype_t)nk_e3m2_k;
    case scalar_kind_t::e2m3_k: return (nk_dtype_t)nk_e2m3_k;
    case scalar_kind_t::i8_k: return (nk_dtype_t)nk_i8_k;
    case scalar_kind_t::u8_k: return (nk_dtype_t)nk_u8_k;
    case scalar_kind_t::b1x8_k: return (nk_dtype_t)nk_u1_k;
    default: return (nk_dtype_t)0;
    }
}

/**
 *  @brief  One entry in the ISA capability-to-name mapping table.
 */
struct isa_target_t {
    nk_capability_t cap;
    char const* name;
};

/**
 *  @brief  Returns the static table mapping each NumKong capability bit to its human-readable name.
 *          Both `hardware_acceleration_available` and `hardware_acceleration_compiled` use this table
 *          together with the bitmasks from `nk_capabilities_available()` / `nk_capabilities_compiled()`.
 */
inline span_gt<isa_target_t const> isa_targets() noexcept {
    static isa_target_t const table[] = {
        {nk_cap_serial_k, "serial"},
        // x86
        {nk_cap_haswell_k, "haswell"},
        {nk_cap_skylake_k, "skylake"},
        {nk_cap_icelake_k, "icelake"},
        {nk_cap_genoa_k, "genoa"},
        {nk_cap_sapphire_k, "sapphire"},
        {nk_cap_sapphireamx_k, "sapphireamx"},
        {nk_cap_graniteamx_k, "graniteamx"},
        {nk_cap_turin_k, "turin"},
        {nk_cap_sierra_k, "sierra"},
        {nk_cap_alder_k, "alder"},
        {nk_cap_diamond_k, "diamond"},
        // ARM NEON
        {nk_cap_neon_k, "neon"},
        {nk_cap_neonhalf_k, "neonhalf"},
        {nk_cap_neonsdot_k, "neonsdot"},
        {nk_cap_neonbfdot_k, "neonbfdot"},
        {nk_cap_neonfhm_k, "neonfhm"},
        {nk_cap_neonfp8_k, "neonfp8"},
        // ARM SVE
        {nk_cap_sve_k, "sve"},
        {nk_cap_svehalf_k, "svehalf"},
        {nk_cap_svesdot_k, "svesdot"},
        {nk_cap_svebfdot_k, "svebfdot"},
        {nk_cap_sve2_k, "sve2"},
        {nk_cap_sve2p1_k, "sve2p1"},
        // ARM SME
        {nk_cap_sme_k, "sme"},
        {nk_cap_sme2_k, "sme2"},
        {nk_cap_sme2p1_k, "sme2p1"},
        {nk_cap_smef64_k, "smef64"},
        {nk_cap_smefa64_k, "smefa64"},
        {nk_cap_smehalf_k, "smehalf"},
        {nk_cap_smebf16_k, "smebf16"},
        {nk_cap_smelut2_k, "smelut2"},
        {nk_cap_smebi32_k, "smebi32"},
        // RISC-V
        {nk_cap_rvv_k, "rvv"},
        {nk_cap_rvvhalf_k, "rvvhalf"},
        {nk_cap_rvvbf16_k, "rvvbf16"},
        {nk_cap_rvvbb_k, "rvvbb"},
        // WebAssembly
        {nk_cap_v128relaxed_k, "v128relaxed"},
        // LoongArch
        {nk_cap_loongsonasx_k, "loongsonasx"},
        // IBM Power
        {nk_cap_powervsx_k, "powervsx"},
    };
    return {table, sizeof(table) / sizeof(table[0])};
}

/**
 *  @brief  Returns the human-readable name for a single capability bit.
 *  @param  cap  A single `nk_capability_t` bit (not a bitmask of multiple capabilities).
 *  @return The name string, or "unknown" if the bit is not in the table.
 */
inline char const* capability_name(nk_capability_t cap) noexcept {
    span_gt<isa_target_t const> table = isa_targets();
    for (std::size_t i = 0; i != table.size(); ++i)
        if (table[i].cap == cap)
            return table[i].name;
    return "unknown";
}

/**
 *  @brief  Formats all ISA targets whose bits are set in `caps` into a comma-separated string.
 *  @param  caps    Capability bitmask to filter against.
 *  @param  buf     Output buffer.
 *  @param  buf_len Size of the output buffer in bytes.
 *  @return The number of characters written (excluding the null terminator).
 */
inline std::size_t isa_targets_format(nk_capability_t caps, char* buf, std::size_t buf_len) noexcept {
    if (!buf_len)
        return 0;
    span_gt<isa_target_t const> table = isa_targets();
    char* p = buf;
    char* end = buf + buf_len - 1;
    std::size_t matched = 0;
    for (std::size_t i = 0; i != table.size(); ++i) {
        if (!(caps & table[i].cap))
            continue;
        std::size_t name_len = std::strlen(table[i].name);
        std::size_t needed = name_len + (matched ? 2 : 0);
        if (p + needed > end)
            break;
        if (matched++)
            *p++ = ',', *p++ = ' ';
        std::memcpy(p, table[i].name, name_len);
        p += name_len;
    }
    *p = '\0';
    return static_cast<std::size_t>(p - buf);
}

/**
 *  @brief  Returns a comma-separated list of ISAs available at runtime (compiled AND supported by CPU).
 *  @return Pointer to a static null-terminated string. Thread-safe after first call.
 */
inline char const* hardware_acceleration_available() noexcept {
    static char buf[1024];
    static bool initialized = false;
    if (!initialized) {
        nk_cached_capabilities(); // ensures nk_configure_thread is called
        isa_targets_format(nk_capabilities_available(), buf, sizeof(buf));
        initialized = true;
    }
    return buf;
}

/**
 *  @brief  Returns a comma-separated list of ISAs that were compiled into this binary.
 *  @return Pointer to a static null-terminated string. Thread-safe after first call.
 */
inline char const* hardware_acceleration_compiled() noexcept {
    static char buf[1024];
    static bool initialized = false;
    if (!initialized) {
        isa_targets_format(nk_capabilities_compiled(), buf, sizeof(buf));
        initialized = true;
    }
    return buf;
}

#else

inline char const* hardware_acceleration_available() noexcept { return "serial"; }
inline char const* hardware_acceleration_compiled() noexcept { return "serial"; }

#endif

} // namespace usearch
} // namespace unum
