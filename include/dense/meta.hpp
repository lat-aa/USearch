/**
 *  @file       meta.hpp
 *  @brief      稠密索引文件头、metadata 读写与版本修复。
 */
#pragma once
#include <dense/config.hpp>
namespace unum {
namespace usearch {

template <typename, typename> class index_dense_gt;

/**
 *  @brief  The "magic" sequence helps infer the type of the file.
 *          USearch indexes start with the "usearch" string.
 */
constexpr char const* default_magic() { return "usearch"; }

using index_dense_head_buffer_t = byte_t[64];

static_assert(sizeof(index_dense_head_buffer_t) == 64, "File header should be exactly 64 bytes");

/**
 *  @brief  Serialized binary representations of the USearch index start with metadata.
 *          Metadata is parsed into a `index_dense_head_t`, containing the USearch package version,
 *          and the properties of the index.
 *
 *  It uses: 13 bytes for file versioning, 22 bytes for structural information = 35 bytes.
 *  The following 24 bytes contain binary size of the graph, of the vectors, and the checksum,
 *  leaving 5 bytes at the end vacant.
 */
struct index_dense_head_t {

    // Versioning:
    using magic_t = char[7];
    using version_t = std::uint16_t;

    // Versioning: 7 + 2 * 3 = 13 bytes
    char const* magic;
    misaligned_ref_gt<version_t> version_major;
    misaligned_ref_gt<version_t> version_minor;
    misaligned_ref_gt<version_t> version_patch;

    // Structural: 4 * 3 = 12 bytes
    misaligned_ref_gt<metric_kind_t> kind_metric;
    misaligned_ref_gt<scalar_kind_t> kind_scalar;
    misaligned_ref_gt<scalar_kind_t> kind_key;
    misaligned_ref_gt<scalar_kind_t> kind_compressed_slot;

    // Population: 8 * 3 = 24 bytes
    misaligned_ref_gt<std::uint64_t> count_present;
    misaligned_ref_gt<std::uint64_t> count_deleted;
    misaligned_ref_gt<std::uint64_t> dimensions;
    misaligned_ref_gt<bool> multi;

    index_dense_head_t(byte_t* ptr) noexcept
        : magic((char const*)exchange(ptr, ptr + sizeof(magic_t))),         //
          version_major(exchange(ptr, ptr + sizeof(version_t))),            //
          version_minor(exchange(ptr, ptr + sizeof(version_t))),            //
          version_patch(exchange(ptr, ptr + sizeof(version_t))),            //
          kind_metric(exchange(ptr, ptr + sizeof(metric_kind_t))),          //
          kind_scalar(exchange(ptr, ptr + sizeof(scalar_kind_t))),          //
          kind_key(exchange(ptr, ptr + sizeof(scalar_kind_t))),             //
          kind_compressed_slot(exchange(ptr, ptr + sizeof(scalar_kind_t))), //
          count_present(exchange(ptr, ptr + sizeof(std::uint64_t))),        //
          count_deleted(exchange(ptr, ptr + sizeof(std::uint64_t))),        //
          dimensions(exchange(ptr, ptr + sizeof(std::uint64_t))),           //
          multi(exchange(ptr, ptr + sizeof(bool))) {}
};

struct index_dense_head_result_t {

    index_dense_head_buffer_t buffer;
    index_dense_head_t head;
    error_t error;

    explicit operator bool() const noexcept { return !error; }
    index_dense_head_result_t failed(error_t message) noexcept {
        error = std::move(message);
        return std::move(*this);
    }
};

struct index_dense_metadata_result_t {
    index_dense_serialization_config_t config;
    index_dense_head_buffer_t head_buffer;
    index_dense_head_t head;
    error_t error;

    explicit operator bool() const noexcept { return !error; }
    index_dense_metadata_result_t failed(error_t message) noexcept {
        error = std::move(message);
        return std::move(*this);
    }

    index_dense_metadata_result_t() noexcept : config(), head_buffer(), head(head_buffer), error() {}

    index_dense_metadata_result_t(index_dense_metadata_result_t&& other) noexcept
        : config(), head_buffer(), head(head_buffer), error(std::move(other.error)) {
        std::memcpy(&config, &other.config, sizeof(other.config));
        std::memcpy(&head_buffer, &other.head_buffer, sizeof(other.head_buffer));
    }

    index_dense_metadata_result_t& operator=(index_dense_metadata_result_t&& other) noexcept {
        std::memcpy(&config, &other.config, sizeof(other.config));
        std::memcpy(&head_buffer, &other.head_buffer, sizeof(other.head_buffer));
        error = std::move(other.error);
        return *this;
    }
};

/**
 *  @brief  Fixes serialized scalar-kind codes for pre-v2.10 versions, until we can upgrade to v3.
 *          The old enum `scalar_kind_t` is defined without explicit constants from 0.
 */
inline scalar_kind_t convert_pre_2_10_scalar_kind(scalar_kind_t scalar_kind) noexcept {
    switch (static_cast<std::underlying_type<scalar_kind_t>::type>(scalar_kind)) {
    case 0: return scalar_kind_t::unknown_k;
    case 1: return scalar_kind_t::b1x8_k;
    case 2: return scalar_kind_t::u40_k;
    case 3: return scalar_kind_t::uuid_k;
    case 4: return scalar_kind_t::f64_k;
    case 5: return scalar_kind_t::f32_k;
    case 6: return scalar_kind_t::f16_k;
    case 7: return scalar_kind_t::e5m2_k;
    case 8: return scalar_kind_t::u64_k;
    case 9: return scalar_kind_t::u32_k;
    case 10: return scalar_kind_t::u8_k;
    case 11: return scalar_kind_t::i64_k;
    case 12: return scalar_kind_t::i32_k;
    case 13: return scalar_kind_t::i16_k;
    case 14: return scalar_kind_t::i8_k;
    default: return scalar_kind;
    }
}

/**
 *  @brief  Fixes the metadata for pre-v2.10 versions, until we can upgrade to v3.
 *          Originates from: https://github.com/unum-cloud/USearch/issues/423
 */
inline void fix_pre_2_10_metadata(index_dense_head_t& head) {
    if (head.version_major == 2 && head.version_minor < 10) {
        head.kind_scalar = convert_pre_2_10_scalar_kind(head.kind_scalar);
        head.kind_key = convert_pre_2_10_scalar_kind(head.kind_key);
        head.kind_compressed_slot = convert_pre_2_10_scalar_kind(head.kind_compressed_slot);
        head.version_minor = 10;
        head.version_patch = 0;
    }
}

/**
 *  @brief  Extracts metadata from a pre-constructed index on disk,
 *          without loading it or mapping the whole binary file.
 */
inline index_dense_metadata_result_t index_dense_metadata_from_path(char const* file_path) noexcept {
    index_dense_metadata_result_t result;
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(file_path, "rb"), &std::fclose);
    if (!file)
        return result.failed(std::strerror(errno));

    // Read the header
    std::size_t read = std::fread(result.head_buffer, sizeof(index_dense_head_buffer_t), 1, file.get());
    if (!read)
        return result.failed(std::feof(file.get()) ? "End of file reached!" : std::strerror(errno));

    // Check if the file immediately starts with the index, instead of vectors
    result.config.exclude_vectors = true;
    if (std::memcmp(result.head_buffer, default_magic(), std::strlen(default_magic())) == 0) {
        fix_pre_2_10_metadata(result.head);
        return result;
    }

    if (std::fseek(file.get(), 0L, SEEK_END) != 0)
        return result.failed("Can't infer file size");

    // Check if it starts with 32-bit
    std::size_t const file_size = std::ftell(file.get());

    std::uint32_t dimensions_u32[2]{0};
    std::memcpy(dimensions_u32, result.head_buffer, sizeof(dimensions_u32));
    checked_size_result_t offset_if_u32 =
        checked_mul_add(std::size_t(dimensions_u32[0]), std::size_t(dimensions_u32[1]), sizeof(dimensions_u32));

    std::uint64_t dimensions_u64[2]{0};
    std::memcpy(dimensions_u64, result.head_buffer, sizeof(dimensions_u64));
    checked_size_result_t rows_if_u64 = checked_size_from_u64(dimensions_u64[0]);
    checked_size_result_t columns_if_u64 = checked_size_from_u64(dimensions_u64[1]);
    checked_size_result_t offset_if_u64 =
        rows_if_u64 && columns_if_u64 ? checked_mul_add(rows_if_u64.value, columns_if_u64.value, sizeof(dimensions_u64))
                                      : checked_size_overflow();

    // Check if it starts with 32-bit
    checked_size_result_t head_offset_if_u32 =
        offset_if_u32 ? checked_add(offset_if_u32.value, sizeof(index_dense_head_buffer_t)) : offset_if_u32;
    if (head_offset_if_u32 && head_offset_if_u32.value < file_size) {
        if (std::fseek(file.get(), static_cast<long>(offset_if_u32.value), SEEK_SET) != 0)
            return result.failed(std::strerror(errno));
        read = std::fread(result.head_buffer, sizeof(index_dense_head_buffer_t), 1, file.get());
        if (!read)
            return result.failed(std::feof(file.get()) ? "End of file reached!" : std::strerror(errno));

        result.config.exclude_vectors = false;
        result.config.use_64_bit_dimensions = false;
        if (std::memcmp(result.head_buffer, default_magic(), std::strlen(default_magic())) == 0) {
            fix_pre_2_10_metadata(result.head);
            return result;
        }
    }

    // Check if it starts with 64-bit
    checked_size_result_t head_offset_if_u64 =
        offset_if_u64 ? checked_add(offset_if_u64.value, sizeof(index_dense_head_buffer_t)) : offset_if_u64;
    if (head_offset_if_u64 && head_offset_if_u64.value < file_size) {
        if (std::fseek(file.get(), static_cast<long>(offset_if_u64.value), SEEK_SET) != 0)
            return result.failed(std::strerror(errno));
        read = std::fread(result.head_buffer, sizeof(index_dense_head_buffer_t), 1, file.get());
        if (!read)
            return result.failed(std::feof(file.get()) ? "End of file reached!" : std::strerror(errno));

        // Check if it starts with 64-bit
        result.config.exclude_vectors = false;
        result.config.use_64_bit_dimensions = true;
        if (std::memcmp(result.head_buffer, default_magic(), std::strlen(default_magic())) == 0) {
            fix_pre_2_10_metadata(result.head);
            return result;
        }
    }

    return result.failed("Not a dense USearch index!");
}

/**
 *  @brief  Extracts metadata from a pre-constructed index serialized into an in-memory buffer.
 */
inline index_dense_metadata_result_t index_dense_metadata_from_buffer(memory_mapped_file_t const& file,
                                                                      std::size_t offset = 0) noexcept {
    index_dense_metadata_result_t result;

    // Read the header
    if (offset + sizeof(index_dense_head_buffer_t) >= file.size())
        return result.failed("End of file reached!");

    byte_t const* file_data = file.data() + offset;
    std::size_t const file_size = file.size() - offset;
    std::memcpy(&result.head_buffer, file_data, sizeof(index_dense_head_buffer_t));

    // Check if the file immediately starts with the index, instead of vectors
    result.config.exclude_vectors = true;
    if (std::memcmp(result.head_buffer, default_magic(), std::strlen(default_magic())) == 0)
        return result;

    // Check if it starts with 32-bit
    std::uint32_t dimensions_u32[2]{0};
    std::memcpy(dimensions_u32, result.head_buffer, sizeof(dimensions_u32));
    checked_size_result_t offset_if_u32 =
        checked_mul_add(std::size_t(dimensions_u32[0]), std::size_t(dimensions_u32[1]), sizeof(dimensions_u32));

    std::uint64_t dimensions_u64[2]{0};
    std::memcpy(dimensions_u64, result.head_buffer, sizeof(dimensions_u64));
    checked_size_result_t rows_if_u64 = checked_size_from_u64(dimensions_u64[0]);
    checked_size_result_t columns_if_u64 = checked_size_from_u64(dimensions_u64[1]);
    checked_size_result_t offset_if_u64 =
        rows_if_u64 && columns_if_u64 ? checked_mul_add(rows_if_u64.value, columns_if_u64.value, sizeof(dimensions_u64))
                                      : checked_size_overflow();

    // Check if it starts with 32-bit
    checked_size_result_t head_offset_if_u32 =
        offset_if_u32 ? checked_add(offset_if_u32.value, sizeof(index_dense_head_buffer_t)) : offset_if_u32;
    if (head_offset_if_u32 && head_offset_if_u32.value < file_size) {
        std::memcpy(&result.head_buffer, file_data + offset_if_u32.value, sizeof(index_dense_head_buffer_t));
        result.config.exclude_vectors = false;
        result.config.use_64_bit_dimensions = false;
        if (std::memcmp(result.head_buffer, default_magic(), std::strlen(default_magic())) == 0)
            return result;
    }

    // Check if it starts with 64-bit
    checked_size_result_t head_offset_if_u64 =
        offset_if_u64 ? checked_add(offset_if_u64.value, sizeof(index_dense_head_buffer_t)) : offset_if_u64;
    if (head_offset_if_u64 && head_offset_if_u64.value < file_size) {
        std::memcpy(&result.head_buffer, file_data + offset_if_u64.value, sizeof(index_dense_head_buffer_t));
        result.config.exclude_vectors = false;
        result.config.use_64_bit_dimensions = true;
        if (std::memcmp(result.head_buffer, default_magic(), std::strlen(default_magic())) == 0)
            return result;
    }

    return result.failed("Not a dense USearch index!");
}

} // namespace usearch
} // namespace unum
