/**
 *  @file       io.hpp
 *  @brief      序列化结果、FILE 读写封装与 memory_mapped_file_t。
 */
#pragma once
#include <usearch/stub.hpp>
namespace unum {
namespace usearch {

struct serialization_result_t {
    error_t error;

    explicit operator bool() const noexcept { return !error; }
    serialization_result_t failed(error_t message) noexcept {
        error = std::move(message);
        return std::move(*this);
    }
};

/**
 *  @brief Smart-pointer wrapping the LibC @b `FILE` for binary file @b outputs.
 *
 * This class raises no exceptions and corresponds errors through `serialization_result_t`.
 * The class automatically closes the file when the object is destroyed.
 */
class output_file_t {
    char const* path_ = nullptr;
    std::FILE* file_ = nullptr;

  public:
    output_file_t(char const* path) noexcept : path_(path) {}
    ~output_file_t() noexcept { close(); }
    output_file_t(output_file_t&& other) noexcept
        : path_(exchange(other.path_, nullptr)), file_(exchange(other.file_, nullptr)) {}
    output_file_t& operator=(output_file_t&& other) noexcept {
        std::swap(path_, other.path_);
        std::swap(file_, other.file_);
        return *this;
    }
    serialization_result_t open_if_not() noexcept {
        serialization_result_t result;
        if (!file_)
            file_ = std::fopen(path_, "wb");
        if (!file_)
            return result.failed(std::strerror(errno));
        return result;
    }
    serialization_result_t write(void const* begin, std::size_t length) noexcept {
        serialization_result_t result;
        std::size_t written = std::fwrite(begin, length, 1, file_);
        if (length && !written)
            return result.failed(std::strerror(errno));
        return result;
    }
    void close() noexcept {
        if (file_)
            std::fclose(exchange(file_, nullptr));
    }
};

/**
 *  @brief  Smart-pointer wrapping the LibC @b `FILE` for binary files @b inputs.
 *
 * This class raises no exceptions and corresponds errors through `serialization_result_t`.
 * The class automatically closes the file when the object is destroyed.
 */
class input_file_t {
    char const* path_ = nullptr;
    std::FILE* file_ = nullptr;

  public:
    input_file_t(char const* path) noexcept : path_(path) {}
    ~input_file_t() noexcept { close(); }
    input_file_t(input_file_t&& other) noexcept
        : path_(exchange(other.path_, nullptr)), file_(exchange(other.file_, nullptr)) {}
    input_file_t& operator=(input_file_t&& other) noexcept {
        std::swap(path_, other.path_);
        std::swap(file_, other.file_);
        return *this;
    }

    serialization_result_t open_if_not() noexcept {
        serialization_result_t result;
        if (!file_)
            file_ = std::fopen(path_, "rb");
        if (!file_)
            return result.failed(std::strerror(errno));
        return result;
    }
    serialization_result_t read(void* begin, std::size_t length) noexcept {
        serialization_result_t result;
        std::size_t read = std::fread(begin, length, 1, file_);
        if (length && !read) {
            bool reached_eof = std::feof(file_);
            return result.failed(reached_eof ? "End of file reached!" : std::strerror(errno));
        }
        return result;
    }
    void close() noexcept {
        if (file_)
            std::fclose(exchange(file_, nullptr));
    }

    explicit operator bool() const noexcept { return file_; }
    bool seek_to(std::size_t progress) noexcept {
        return std::fseek(file_, static_cast<long>(progress), SEEK_SET) == 0;
    }
    bool seek_to_end() noexcept { return std::fseek(file_, 0L, SEEK_END) == 0; }
    bool infer_progress(std::size_t& progress) noexcept {
        long int result = std::ftell(file_);
        if (result == -1L)
            return false;
        progress = static_cast<std::size_t>(result);
        return true;
    }
};

/**
 *  @brief  Represents a memory-mapped file or a pre-allocated anonymous memory region.
 *
 *  This class provides a convenient way to memory-map a file and access its contents as a block of
 *  memory. The class handles platform-specific memory-mapping operations on Windows, Linux, and MacOS.
 *  The class automatically closes the file when the object is destroyed.
 */
class memory_mapped_file_t {
    char const* path_{}; /**< The path to the file to be memory-mapped. */
    void* ptr_{};        /**< A pointer to the memory-mapping. */
    size_t length_{};    /**< The length of the memory-mapped file in bytes. */

#if defined(USEARCH_DEFINED_WINDOWS)
    HANDLE file_handle_{};    /**< The file handle on Windows. */
    HANDLE mapping_handle_{}; /**< The mapping handle on Windows. */
#else
    int file_descriptor_{}; /**< The file descriptor on Linux and MacOS. */
#endif

  public:
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    byte_t* data() noexcept { return reinterpret_cast<byte_t*>(ptr_); }
    byte_t const* data() const noexcept { return reinterpret_cast<byte_t const*>(ptr_); }
    std::size_t size() const noexcept { return static_cast<std::size_t>(length_); }

    memory_mapped_file_t() noexcept {}
    memory_mapped_file_t(char const* path) noexcept : path_(path) {}
    ~memory_mapped_file_t() noexcept { close(); }
    memory_mapped_file_t(memory_mapped_file_t&& other) noexcept
        : path_(exchange(other.path_, nullptr)), ptr_(exchange(other.ptr_, nullptr)),
          length_(exchange(other.length_, 0)),
#if defined(USEARCH_DEFINED_WINDOWS)
          file_handle_(exchange(other.file_handle_, nullptr)), mapping_handle_(exchange(other.mapping_handle_, nullptr))
#else
          file_descriptor_(exchange(other.file_descriptor_, 0))
#endif
    {
    }

    memory_mapped_file_t(memory_mapped_file_t const&) = delete;
    memory_mapped_file_t& operator=(memory_mapped_file_t const&) = delete;

    memory_mapped_file_t(byte_t* data, std::size_t length) noexcept : ptr_(data), length_(length) {}

    memory_mapped_file_t& operator=(memory_mapped_file_t&& other) noexcept {
        std::swap(path_, other.path_);
        std::swap(ptr_, other.ptr_);
        std::swap(length_, other.length_);
#if defined(USEARCH_DEFINED_WINDOWS)
        std::swap(file_handle_, other.file_handle_);
        std::swap(mapping_handle_, other.mapping_handle_);
#else
        std::swap(file_descriptor_, other.file_descriptor_);
#endif
        return *this;
    }

    serialization_result_t open_if_not() noexcept {
        serialization_result_t result;
        if (!path_ || ptr_)
            return result;

#if defined(USEARCH_DEFINED_WINDOWS)

        HANDLE file_handle =
            CreateFileA(path_, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        if (file_handle == INVALID_HANDLE_VALUE)
            return result.failed("Opening file failed!");

        std::size_t file_length = GetFileSize(file_handle, 0);
        HANDLE mapping_handle = CreateFileMapping(file_handle, 0, PAGE_READONLY, 0, 0, 0);
        if (mapping_handle == 0) {
            CloseHandle(file_handle);
            return result.failed("Mapping file failed!");
        }

        byte_t* file = (byte_t*)MapViewOfFile(mapping_handle, FILE_MAP_READ, 0, 0, file_length);
        if (file == 0) {
            CloseHandle(mapping_handle);
            CloseHandle(file_handle);
            return result.failed("View the map failed!");
        }
        file_handle_ = file_handle;
        mapping_handle_ = mapping_handle;
        ptr_ = file;
        length_ = file_length;
#else

#if defined(USEARCH_DEFINED_LINUX)
        int descriptor = open(path_, O_RDONLY | O_NOATIME);
        if (descriptor < 0 && errno == EPERM)
            // `O_NOATIME` requires owning the file or holding `CAP_FOWNER` (see `open(2)`).
            // Retry without it instead of failing outright.
            descriptor = open(path_, O_RDONLY);
#else
        int descriptor = open(path_, O_RDONLY);
#endif
        if (descriptor < 0)
            return result.failed(std::strerror(errno));

        // Estimate the file size
        struct stat file_stat;
        int fstat_status = fstat(descriptor, &file_stat);
        if (fstat_status < 0) {
            ::close(descriptor);
            return result.failed(std::strerror(errno));
        }

        // Map the entire file
        byte_t* file = (byte_t*)mmap(NULL, file_stat.st_size, PROT_READ, MAP_SHARED, descriptor, 0);
        if (file == MAP_FAILED) {
            ::close(descriptor);
            return result.failed(std::strerror(errno));
        }
        file_descriptor_ = descriptor;
        ptr_ = file;
        length_ = file_stat.st_size;
#endif // Platform specific code
        return result;
    }

    void close() noexcept {
        if (!path_) {
            ptr_ = nullptr;
            length_ = 0;
            return;
        }
#if defined(USEARCH_DEFINED_WINDOWS)
        UnmapViewOfFile(ptr_);
        CloseHandle(mapping_handle_);
        CloseHandle(file_handle_);
        mapping_handle_ = nullptr;
        file_handle_ = nullptr;
#else
        munmap(ptr_, length_);
        ::close(file_descriptor_);
        file_descriptor_ = 0;
#endif
        ptr_ = nullptr;
        length_ = 0;
    }
};

} // namespace usearch
} // namespace unum
