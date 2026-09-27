/**
 *  @file   cpp/bench.cpp
 *  @brief  USearch 索引构建吞吐与 ANN 检索召回基准。
 *
 *  本文件只做「可复现对比」：mmap 数据集 →（可选）建索引 → 批量检索 → Recall@k；
 *  不负责生产服务路径。量化度量走 `metric_punned_t`（可含 NumKong）。
 */

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
// 先禁 Windows.h 的 min/max 宏，避免与 std::min/max 冲突。
#define NOMINMAX
#define _USE_MATH_DEFINES

#include <Windows.h>

#include <DbgHelp.h>
#pragma comment(lib, "Dbghelp.lib")

#define STDERR_FILENO HANDLE(2)
#else
#if defined(__linux__)
#include <execinfo.h> // backtrace / 崩溃栈
#endif
#include <fcntl.h>    // open
#include <stdlib.h>   // getenv
#include <sys/mman.h> // mmap：零拷贝读 .fbin 等矩阵文件
#include <unistd.h>
#endif

#include <sys/stat.h> // fstat：拿到 mmap 长度

#include <csignal>
#include <cstdio>

#include <algorithm>
#include <iostream>
#include <numeric> // iota：自检索时把「向量 i 的真邻」建成 {i}
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <clipp.h> // CLI 解析
#if USEARCH_USE_OPENMP
#include <omp.h>
#endif

#include <dense/dense.hpp>

using namespace unum::usearch;
using namespace unum;

using compressed_slot_t = std::uint32_t;
using float_span_t = span_gt<float const>;

/// 线性扫描找首个相等元素的偏移；未找到返回 `end - begin`（与 STL 距离语义对齐）。
template <typename element_at>
std::size_t offset_of(element_at const* begin, element_at const* end, element_at v) noexcept {
    auto iterator = begin;
    for (; iterator != end; ++iterator)
        if (*iterator == v)
            break;
    return iterator - begin;
}

template <typename element_at> bool contains(element_at const* begin, element_at const* end, element_at v) noexcept {
    return offset_of(begin, end, v) != static_cast<std::size_t>(end - begin);
}

/**
 *  磁盘矩阵的 mmap 视图：文件头 `rows:u32, cols:u32`，其后 row-major 标量。
 *  对齐 32 字节仅为减少误用栈上拷贝时的对齐踩踏；生命周期与映射绑定，禁止默认拷贝依赖。
 */
template <typename scalar_at> //
struct alignas(32) persisted_matrix_gt {
    using scalar_t = scalar_at;
    std::uint8_t const* raw_handle{};
    std::size_t raw_length{};
    std::uint32_t rows{};
    std::uint32_t cols{};
    scalar_t const* scalars{};

    persisted_matrix_gt() {}

    /// 空路径 → 空矩阵（自检索场景可不提供 queries/neighbors）。
    persisted_matrix_gt(char const* path) noexcept(false) {
        if (!path || !std::strlen(path))
            return;
#if defined(USEARCH_DEFINED_WINDOWS)

        HANDLE file_handle =
            CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (file_handle == INVALID_HANDLE_VALUE)
            throw std::invalid_argument("Couldn't open provided file path");

        LARGE_INTEGER file_size;
        if (!GetFileSizeEx(file_handle, &file_size))
            throw std::invalid_argument("Couldn't obtain file stats");

        raw_length = file_size.QuadPart;
        HANDLE mapping_handle = CreateFileMapping(file_handle, nullptr, PAGE_READONLY, 0, 0, nullptr);

        if (mapping_handle == nullptr)
            throw std::invalid_argument("Couldn't create file mapping");

        raw_handle = (std::uint8_t*)MapViewOfFile(mapping_handle, FILE_MAP_READ, 0, 0, raw_length);

        if (raw_handle == nullptr)
            throw std::invalid_argument("Couldn't memory-map the file");

        std::memcpy(&rows, raw_handle, sizeof(rows));
        std::memcpy(&cols, raw_handle + sizeof(rows), sizeof(cols));
        scalars = (scalar_t*)(raw_handle + sizeof(rows) + sizeof(cols));

#else
        auto file_descriptor = open(path, O_RDONLY | O_CLOEXEC);
        if (file_descriptor == -1)
            throw std::invalid_argument("Couldn't open provided file path");
        struct stat stat_vectors;
        if (fstat(file_descriptor, &stat_vectors) == -1)
            throw std::invalid_argument("Couldn't obtain file stats");
        raw_length = stat_vectors.st_size;
        // MAP_PRIVATE：基准只读，避免写回污染数据集文件。
        auto* result = mmap(NULL, raw_length, PROT_READ, MAP_PRIVATE, file_descriptor, 0);
        if (result == MAP_FAILED)
            throw std::invalid_argument("Couldn't memory-map the file");
        raw_handle = (std::uint8_t*)result;
        std::memcpy(&rows, raw_handle, sizeof(rows));
        std::memcpy(&cols, raw_handle + sizeof(rows), sizeof(cols));
        scalars = (scalar_t*)(raw_handle + sizeof(rows) + sizeof(cols));
#endif // WINDOWS
    }

    ~persisted_matrix_gt() {
        if (raw_handle != nullptr)
#if defined(USEARCH_DEFINED_WINDOWS)
            UnmapViewOfFile(raw_handle);
#else
            munmap((void*)raw_handle, raw_length);
#endif // WINDOWS
    }

    scalar_t const* row(std::size_t i) const noexcept { return scalars + i * cols; }
    std::size_t row_size_bytes() const noexcept { return cols * sizeof(scalar_t); }
    std::size_t size_bytes() const noexcept { return rows * row_size_bytes(); }
};

/**
 *  外部数据集视图：可带 queries + ground-truth neighbors；缺省时做「自检索」
 *  （query=库内向量，真邻={自身}），用于无 GT 文件时仍能估 recall。
 *
 *  `vectors_to_skip` / `vectors_to_take` 切切片，避免改原始 .fbin。
 */
template <typename scalar_at, typename vector_id_at> //
struct persisted_dataset_gt {
    using scalar_t = scalar_at;
    using compressed_slot_t = vector_id_at;
    persisted_matrix_gt<scalar_t> vectors_;
    persisted_matrix_gt<scalar_t> queries_;
    persisted_matrix_gt<compressed_slot_t> neighborhoods_;
    std::vector<default_key_t> vector_ids_;
    std::vector<compressed_slot_t> neighborhoods_iota_{};
    std::size_t vectors_to_skip_{};
    std::size_t vectors_to_take_{};

    persisted_dataset_gt(char const* path_vectors, std::size_t vectors_to_skip = 0,
                         std::size_t vectors_to_take = 0) noexcept(false)
        : vectors_(path_vectors), queries_(), neighborhoods_(), vector_ids_(), vectors_to_skip_(vectors_to_skip),
          vectors_to_take_(vectors_to_take) {
        // 自检索：第 i 条查询的唯一真邻是 i 本身。
        neighborhoods_iota_.resize(vectors_.rows);
        std::iota(neighborhoods_iota_.begin(), neighborhoods_iota_.end(), 0);
    }

    persisted_dataset_gt(char const* path_vectors, char const* path_queries, char const* path_neighbors,
                         std::size_t vectors_to_skip = 0, std::size_t vectors_to_take = 0) noexcept(false)
        : vectors_(path_vectors), queries_(path_queries), neighborhoods_(path_neighbors), vector_ids_(),
          neighborhoods_iota_(), vectors_to_skip_(vectors_to_skip), vectors_to_take_(vectors_to_take) {

        if (vectors_.cols != queries_.cols)
            throw std::invalid_argument("Contents and queries have different dimensionality");
        if (queries_.rows != neighborhoods_.rows)
            throw std::invalid_argument("Number of ground-truth neighborhoods doesn't match number of queries");
    }

    persisted_dataset_gt(char const* path_vectors, char const* path_queries, char const* path_neighbors,
                         char const* path_ids, std::size_t vectors_to_skip = 0,
                         std::size_t vectors_to_take = 0) noexcept(false)
        : vectors_(path_vectors), queries_(path_queries), neighborhoods_(path_neighbors), vector_ids_(),
          neighborhoods_iota_(), vectors_to_skip_(vectors_to_skip), vectors_to_take_(vectors_to_take) {

        // 仅提供库向量、未给 query/GT：退化为自检索。
        if (!queries_.scalars && !neighborhoods_.scalars) {
            neighborhoods_iota_.resize(vectors_.rows);
            std::iota(neighborhoods_iota_.begin(), neighborhoods_iota_.end(), 0);
        } else {
            if (vectors_.cols != queries_.cols)
                throw std::invalid_argument("Contents and queries have different dimensionality");
            if (queries_.rows != neighborhoods_.rows)
                throw std::invalid_argument("Number of ground-truth neighborhoods doesn't match number of queries");
        }

        // 可选外部 ID：键空间非 [0,n) 时，召回比较必须用业务 ID 而非槽位下标。
        if (path_ids && std::strlen(path_ids)) {
            persisted_matrix_gt<std::int32_t> ids_matrix(path_ids);
            if (ids_matrix.rows != vectors_.rows)
                throw std::invalid_argument("Number of vector IDs doesn't match number of vectors");
            if (ids_matrix.cols != 1)
                throw std::invalid_argument("Vector IDs file should have exactly 1 column");

            vector_ids_.resize(ids_matrix.rows);
            for (std::size_t i = 0; i < ids_matrix.rows; ++i)
                vector_ids_[i] = static_cast<default_key_t>(*ids_matrix.row(i));
        }

        // 自检索 + 自定义 ID：iota 真邻改为业务 ID，否则 recall 会拿下标去比 ID。
        if (has_vector_ids() && !queries_.scalars && !neighborhoods_.scalars) {
            for (std::size_t i = 0; i < neighborhoods_iota_.size(); ++i)
                neighborhoods_iota_[i] = static_cast<compressed_slot_t>(vector_ids_[i]);
        }
    }

    bool search_itself() const noexcept { return vectors_count() && !queries_.rows; }
    bool has_vector_ids() const noexcept { return !vector_ids_.empty(); }

    default_key_t vector_id(std::size_t i) const noexcept {
        return has_vector_ids() ? vector_ids_[i + vectors_to_skip_] : static_cast<default_key_t>(i + vectors_to_skip_);
    }

    std::size_t dimensions() const noexcept { return vectors_.cols; }
    std::size_t queries_count() const noexcept { return search_itself() ? vectors_count() : queries_.rows; }
    /// 自检索时邻域宽度固定为 1（只验证「能否找回自己」）。
    std::size_t neighborhood_size() const noexcept { return search_itself() ? 1 : neighborhoods_.cols; }
    scalar_t const* vector(std::size_t i) const noexcept { return vectors_.row(i + vectors_to_skip_); }
    scalar_t const* query(std::size_t i) const noexcept {
        return search_itself() ? vectors_view().at(i) : queries_.row(i);
    }
    compressed_slot_t const* neighborhood(std::size_t i) const noexcept {
        return search_itself() ? neighborhoods_iota_.data() + i : neighborhoods_.row(i);
    }
    std::size_t vectors_count() const noexcept {
        return vectors_to_take_ ? vectors_to_take_ : (vectors_.rows - vectors_to_skip_);
    }
    matrix_slice_gt<scalar_t const> vectors_view() const noexcept { return {vector(0), vectors_count(), dimensions()}; }
};

/// 堆上合成数据集；当前入口未用，保留作无磁盘文件的烟雾基准。
template <typename scalar_at, typename vector_id_at> //
struct in_memory_dataset_gt {
    using scalar_t = scalar_at;
    using compressed_slot_t = vector_id_at;

    std::vector<scalar_t> vectors_{};
    std::vector<scalar_t> queries_{};
    std::vector<compressed_slot_t> neighborhoods_{};
    std::size_t dimensions_{};
    std::size_t vectors_count_{};
    std::size_t neighborhood_size_{};
    std::size_t queries_count_{};

    in_memory_dataset_gt( //
        std::size_t dimensions, std::size_t vectors_count, std::size_t queries_count,
        std::size_t neighborhood_size) noexcept(false)
        : vectors_(vectors_count * dimensions), queries_(queries_count * dimensions),
          neighborhoods_(queries_count * neighborhood_size), dimensions_(dimensions), vectors_count_(vectors_count),
          queries_count_(queries_count), neighborhood_size_(neighborhood_size) {}

    std::size_t dimensions() const noexcept { return dimensions_; }
    std::size_t vectors_count() const noexcept { return vectors_count_; }
    std::size_t queries_count() const noexcept { return vectors_count(); }
    std::size_t neighborhood_size() const noexcept { return 1; }
    default_key_t vector_id(std::size_t i) const noexcept { return static_cast<default_key_t>(i); }
    scalar_t const* vector(std::size_t i) const noexcept { return vectors_.data() + i * dimensions_; }
    scalar_t const* query(std::size_t i) const noexcept { return queries_.data() + i * dimensions_; }
    compressed_slot_t const* neighborhood(std::size_t i) const noexcept {
        return neighborhoods_.data() + i * neighborhood_size_;
    }

    scalar_t* vector(std::size_t i) noexcept { return vectors_.data() + i * dimensions_; }
    scalar_t* query(std::size_t i) noexcept { return queries_.data() + i * dimensions_; }
    compressed_slot_t* neighborhood(std::size_t i) noexcept { return neighborhoods_.data() + i * neighborhood_size_; }

    matrix_slice_gt<scalar_t const> vectors_view() const noexcept { return {vector(0), vectors_count(), dimensions()}; }
};

char const* getenv_or(char const* name, char const* default_) { return getenv(name) ? getenv(name) : default_; }

using timestamp_t = std::chrono::time_point<std::chrono::high_resolution_clock>;

/**
 *  进度条：按步长节流打印，避免 OpenMP 热路径上每条向量都刷屏。
 *  吞吐用「自上次打印以来」的局部速率，避免全程平均掩盖尾部变慢。
 */
struct running_stats_printer_t {
    std::size_t total{};
    std::atomic<std::size_t> progress{};
    std::size_t last_printed_progress{};
    timestamp_t last_printed_time{};
    timestamp_t start_time{};

    running_stats_printer_t(std::size_t n, char const* msg) {
        std::printf("%s. %zu items\n", msg, n);
        total = n;
        last_printed_time = start_time = std::chrono::high_resolution_clock::now();
    }

    ~running_stats_printer_t() {
        std::size_t count = progress.load();
        timestamp_t time = std::chrono::high_resolution_clock::now();
        std::size_t duration = std::chrono::duration_cast<std::chrono::nanoseconds>(time - start_time).count();
        float vectors_per_second = static_cast<float>(count * 1e9 / duration);
        std::printf("\r\33[2K100 %% completed, %.0f vectors/s\n", vectors_per_second);
    }

    void refresh(std::size_t step = 1024 * 32) {
        std::size_t new_progress = progress.load();
        if (new_progress - last_printed_progress < step)
            return;
        print(new_progress, total);
    }

    void print() { print(progress.load(), total); }

    void print(std::size_t progress, std::size_t total) {

        constexpr char bars_k[] = "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||";
        constexpr std::size_t bars_len_k = 60;

        float percentage = progress * 1.f / total;
        int left_pad = (int)(percentage * bars_len_k);
        int right_pad = bars_len_k - left_pad;

        std::size_t count_new = progress - last_printed_progress;
        timestamp_t time_new = std::chrono::high_resolution_clock::now();
        std::size_t duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(time_new - last_printed_time).count();
        float vectors_per_second = static_cast<float>(count_new * 1e9 / duration);

        std::printf("\r%3.3f%% [%.*s%*s] %.0f vectors/s, finished %zu/%zu", percentage * 100.f, left_pad, bars_k,
                    right_pad, "", vectors_per_second, progress, total);
        std::fflush(stdout);

        last_printed_progress = progress;
        last_printed_time = time_new;
        this->total = total;
    }
};

/// 并行插入；仅 thread 0 刷进度，避免多线程争用 stdout。
template <typename index_at, typename vector_id_at, typename scalar_at>
void index_many(index_at& index, std::size_t n, vector_id_at const* ids, scalar_at const* vectors, std::size_t dims) {

    running_stats_printer_t printer{n, "Indexing"};

#if USEARCH_USE_OPENMP
#pragma omp parallel for schedule(static, 32)
#endif
    for (std::size_t i = 0; i < n; ++i) {
        index_update_config_t config;
#if USEARCH_USE_OPENMP
        config.thread = omp_get_thread_num();
#endif
        index.add(ids[i], vectors + dims * i, config.thread);
        printer.progress++;
        if (config.thread == 0)
            printer.refresh();
    }

    printer.print();
}

/// 并行检索；结果按 query 分块写入调用方缓冲（wanted 对齐）。
template <typename index_at, typename vector_id_at, typename scalar_at, typename distance_at>
void search_many( //
    index_at& index, std::size_t n, scalar_at const* vectors, std::size_t dims, std::size_t wanted, vector_id_at* ids,
    distance_at* distances) {

    std::string name = "Search " + std::to_string(wanted);
    running_stats_printer_t printer{n, name.c_str()};

#if USEARCH_USE_OPENMP
#pragma omp parallel for schedule(static, 32)
#endif
    for (std::size_t i = 0; i < n; ++i) {
        index_search_config_t config;
#if USEARCH_USE_OPENMP
        config.thread = omp_get_thread_num();
#endif
        span_gt<scalar_at const> vector{vectors + dims * i, dims};
        index.search(vector, wanted, config.thread).dump_to(ids + wanted * i, distances + wanted * i, wanted);
        printer.progress++;
        if (config.thread == 0)
            printer.refresh();
    }

    printer.print();
}

/**
 *  单次基准回合：可选建库 → 搜 → Recall@1 / Recall@k；
 *  `bench_join==false` 时额外跑双图 join（与 CLI `--join` 极性历史相反，改名需同步调用方）。
 */
template <typename dataset_at, typename index_at> //
static void single_shot(dataset_at& dataset, index_at& index, bool construct = true, bool bench_join = false) {
    using distance_t = typename index_at::distance_t;

    std::printf("\n");
    std::printf("------------\n");
    if (construct) {
        std::vector<default_key_t> ids(dataset.vectors_count());
        for (std::size_t i = 0; i < dataset.vectors_count(); ++i)
            ids[i] = static_cast<default_key_t>(dataset.vector_id(i));
        index_many(index, dataset.vectors_count(), ids.data(), dataset.vector(0), dataset.dimensions());
    }

    std::size_t mem = index.memory_usage();
    std::printf("Memory usage: %.2f GB\n", mem / (1024.0 * 1024.0 * 1024.0));

    std::vector<default_key_t> found_neighbors(dataset.queries_count() * dataset.neighborhood_size());
    std::vector<distance_t> found_distances(dataset.queries_count() * dataset.neighborhood_size());
    search_many(index, dataset.queries_count(), dataset.query(0), dataset.dimensions(), dataset.neighborhood_size(),
                found_neighbors.data(), found_distances.data());

    // Recall@1：真邻首位是否命中；Recall：真邻首位是否落在返回集合内（允许排序漂移）。
    std::size_t recall_at_1 = 0, recall_full = 0;
    for (std::size_t i = 0; i != dataset.queries_count(); ++i) {
        auto expected = dataset.neighborhood(i);
        auto received = found_neighbors.data() + i * dataset.neighborhood_size();
        recall_at_1 += expected[0] == received[0];
        recall_full += contains(received, received + dataset.neighborhood_size(), default_key_t{expected[0]});
    }

    std::printf("Recall@1 %.2f %%\n", recall_at_1 * 100.f / dataset.queries_count());
    std::printf("Recall %.2f %%\n", recall_full * 100.f / dataset.queries_count());

    if (!bench_join) {
        // 用 map 而非数组：键空间可能不稠密（自定义 ID）。
        std::unordered_map<default_key_t, default_key_t> man_to_woman;
        std::unordered_map<default_key_t, default_key_t> woman_to_man;
        std::size_t join_attempts = 0;

        index_at& men = index;
        index_at women = index.copy();

        executor_default_t executor(index.limits().threads());
        running_stats_printer_t printer{1, "Join"};
        join_result_t result = join(                          //
            men, women, index_join_config_t{executor.size()}, //
            man_to_woman, woman_to_man,                       //
            executor, [&](std::size_t progress, std::size_t total) {
                if (progress % 1000 == 0)
                    printer.print(progress, total);
                return true;
            });
        printer.print();
        join_attempts = result.visited_members;

        // 同构 copy 上的 join：理想匹配应是 id→id；偏离反映近似图误差。
        std::size_t recall_join = 0;
        for (auto const& [man, woman] : man_to_woman)
            recall_join += (man == woman);
        std::size_t unmatched_count = dataset.vectors_count() - man_to_woman.size();
        std::printf("Recall Joins %.2f %%\n", recall_join * 100.f / index.size());
        std::printf("Unmatched %.2f %% (%zu items)\n", unmatched_count * 100.f / index.size(), unmatched_count);
        std::printf("Proposals %.2f / man (%zu total)\n", join_attempts * 1.f / index.size(), join_attempts);
    }

    std::printf("------------\n");
    std::printf("\n");
}

/// SIGSEGV 时尽量打印栈；失败则直接 exit，避免二次崩溃掩盖现场。
void handler(int sig) {
    void* array[10];
    size_t size;

#if defined(USEARCH_DEFINED_WINDOWS)
    size = CaptureStackBackTrace(0, 10, array, NULL);
#elif defined(USEARCH_DEFINED_LINUX)
    size = backtrace(array, 10);
#endif // WINDOWS

    fprintf(stderr, "Error: signal %d:\n", sig);

#if defined(USEARCH_DEFINED_WINDOWS)
    SYMBOL_INFO* symbol = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    for (int i = 0; i < size; i++) {
        SymFromAddr(GetCurrentProcess(), (DWORD64)(array[i]), 0, symbol);
        const char* name = symbol->Name;
        if (name == NULL) {
            name = "<unknown>";
        }
        DWORD bytes_written;
        WriteFile(STDERR_FILENO, name, static_cast<DWORD>(std::strlen(name)), &bytes_written, NULL);
        WriteFile(STDERR_FILENO, "\n", 1, &bytes_written, NULL);
    }
    free(symbol);
#elif defined(USEARCH_DEFINED_LINUX)
    backtrace_symbols_fd(array, size, STDERR_FILENO);
#endif // WINDOWS

    exit(1);
}

bool ends_with(std::string const& value, std::string const& ending) {
    if (ending.size() > value.size())
        return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

struct args_t {
    std::string path_vectors;
    std::string path_queries;
    std::string path_neighbors;
    std::string path_ids;
    std::string path_output = "last.usearch";

    std::size_t connectivity = default_connectivity();
    std::size_t expansion_add = default_expansion_add();
    std::size_t expansion_search = default_expansion_search();
    std::size_t threads = std::thread::hardware_concurrency();

    std::size_t vectors_to_skip = 0;
    std::size_t vectors_to_take = 0;

    bool help = false;

    bool big = false;
    bool join = false;
    bool view = false;

    std::string dtype_str = "f32";
    std::string metric_str = "ip";

    /// 解析失败时回退 ip，避免基准因拼写错误直接中止。
    metric_kind_t metric() const noexcept {
        auto parsed = metric_from_name(metric_str.c_str(), metric_str.size());
        if (!parsed)
            return metric_kind_t::ip_k;
        return parsed.result;
    }

    scalar_kind_t quantization() const noexcept {
        auto parsed = scalar_kind_from_name(dtype_str.c_str(), dtype_str.size());
        if (!parsed)
            return scalar_kind_t::f32_k;
        return parsed.result;
    }
};

/// 量化 + 动态度量（可走 NumKong）；先内存基准，可选再 view 磁盘镜像对比。
template <typename index_at, typename dataset_at> //
void run_punned(dataset_at& dataset, args_t const& args, index_dense_config_t config, index_limits_t limits) {

    scalar_kind_t quantization = args.quantization();
    std::printf("-- Quantization: %s\n", scalar_kind_name(quantization));

    metric_kind_t kind = args.metric();
    std::printf("-- Metric: %s\n", metric_kind_name(kind));

    metric_punned_t metric(dataset.dimensions(), kind, quantization);
    index_at index = index_at::make(metric, config);
    index.reserve(limits);
    std::printf("-- Hardware acceleration: %s\n", index.metric().isa_name());
    std::printf("Will benchmark in-memory\n");

    single_shot(dataset, index, true, args.join);
    index.save(args.path_output.c_str());

    if (!args.view)
        return;
    std::printf("Will benchmark an on-disk view\n");

    // fork+view：度量配置与内存索引一致，隔离「页缓存 / 懒加载」对检索吞吐的影响。
    index_at index_view = index.fork();
    index_view.view(args.path_output.c_str());
    single_shot(dataset, index_view, false, args.join);
}

template <typename index_at, typename dataset_at> //
void run_typed(dataset_at& dataset, args_t const& args, index_config_t config, index_limits_t limits) {

    index_at index(config);
    index.reserve(limits);
    std::printf("Will benchmark in-memory\n");

    single_shot(dataset, index, true, args.join);
    index.save(args.path_output.c_str());

    if (!args.view)
        return;
    std::printf("Will benchmark an on-disk view\n");

    index_at index_view = index.fork();
    index_view.view(args.path_output.c_str());
    single_shot(dataset, index_view, false, args.join);
}

template <typename dataset_scalar_at> void bench_with_args(args_t const& args) {
    using dataset_t = persisted_dataset_gt<dataset_scalar_at, compressed_slot_t>;

    dataset_t dataset(args.path_vectors.c_str(), args.path_queries.c_str(), args.path_neighbors.c_str(),
                      args.path_ids.c_str(), args.vectors_to_skip, args.vectors_to_take);
    std::printf("-- Dimensions: %zu\n", dataset.dimensions());
    std::printf("-- Vectors count: %zu\n", dataset.vectors_count());
    std::printf("-- Queries count: %zu\n", dataset.queries_count());
    std::printf("-- Neighbors per query: %zu\n", dataset.neighborhood_size());

    index_dense_config_t config(args.connectivity, args.expansion_add, args.expansion_search);
    index_limits_t limits;
    // 构建与检索共用线程上限，避免一边抢核导致吞吐抖动。
    limits.threads_add = limits.threads_search = args.threads;
    limits.members = dataset.vectors_count();

    std::printf("- Index: \n");
    std::printf("-- Connectivity: %zu\n", config.connectivity);
    std::printf("-- Expansion @ Add: %zu\n", config.expansion_add);
    std::printf("-- Expansion @ Search: %zu\n", config.expansion_search);

    if (args.big)
#ifdef USEARCH_64BIT_ENV
        // 邻接槽位压缩到 40 bit，支持 >4B 图；仅 64 位进程安全。
        run_punned<index_dense_gt<default_key_t, uint40_t>>(dataset, args, config, limits);
#else
        std::printf("Error: Don't use 40 bit identifiers in 32bit environment\n");
#endif
    else
        run_punned<index_dense_gt<default_key_t, std::uint32_t>>(dataset, args, config, limits);
}

int main(int argc, char** argv) {

    // 尽早挂 SIGSEGV，避免 OpenMP/SIMD 路径崩溃时无栈可查。
    signal(SIGSEGV, handler);

    using namespace clipp;

    auto args = args_t{};
    auto cli = ( //
        (option("--vectors") & value("path", args.path_vectors))
            .doc(".[fhbd]bin, .i8bin, .u8bin, .f32bin file path to construct the index"),
        (option("--queries") & value("path", args.path_queries))
            .doc(".[fhbd]bin, .i8bin, .u8bin, .f32bin file path to query the index"),
        (option("--neighbors") & value("path", args.path_neighbors)).doc(".ibin, .i32bin file path with ground truth"),
        (option("--ids") & value("path", args.path_ids)).doc(".i32bin file path with vector IDs (optional)"),
        (option("-o", "--output") & value("path", args.path_output)).doc(".usearch output file path"),
        (option("-b", "--big").set(args.big)).doc("Will switch to uint40_t for neighbors lists with over 4B entries"),
        (option("-j", "--threads") & value("integer", args.threads)).doc("Uses all available cores by default"),
        (option("-c", "--connectivity") & value("integer", args.connectivity)).doc("Index granularity"),
        (option("--expansion-add") & value("integer", args.expansion_add)).doc("Affects indexing depth"),
        (option("--expansion-search") & value("integer", args.expansion_search)).doc("Affects search depth"),
        (option("--rows-skip") & value("integer", args.vectors_to_skip)).doc("Number of vectors to skip"),
        (option("--rows-take") & value("integer", args.vectors_to_take)).doc("Number of vectors to take"),
        (option("--dtype") & value("type", args.dtype_str))
            .doc("Quantization type: f64, f32, bf16, f16, e5m2, e4m3, e3m2, e2m3, i8, u8, b1"),
        (option("--metric") & value("name", args.metric_str))
            .doc("Distance metric: ip, l2sq, cos, hamming, tanimoto, sorensen, haversine"),
        option("-h", "--help").set(args.help).doc("Print this help information on this tool and exit"),
        option("--join").set(args.join).doc("Also benchmark joins"),
        option("--view").set(args.view).doc("Also benchmark on-disk view"));

    if (!parse(argc, argv, cli)) {
        std::cerr << make_man_page(cli, argv[0]);
        exit(1);
    }
    if (args.help) {
        std::cout << make_man_page(cli, argv[0]);
        exit(0);
    }

#if USEARCH_USE_OPENMP
    // 不用 index_dense 内置线程池：跨 batch 复用 OpenMP，吞吐统计更稳。
    omp_set_dynamic(true);
    omp_set_num_threads(static_cast<int>(args.threads));
    std::printf("- OpenMP threads: %d\n", omp_get_max_threads());
#endif

    std::printf("- Hardware acceleration compiled: %s\n", hardware_acceleration_compiled());
    std::printf("- Hardware acceleration available: %s\n", hardware_acceleration_available());
    std::printf("- Dataset: \n");
    std::printf("-- Base vectors path: %s\n", args.path_vectors.c_str());
    std::printf("-- Query vectors path: %s\n", args.path_queries.c_str());
    std::printf("-- Ground truth neighbors path: %s\n", args.path_neighbors.c_str());

    // 后缀决定元素类型；与 `--dtype`（索引量化）正交——库文件类型 ≠ 索引标量类型。
    auto ends_with = [](std::string_view stack, std::string_view needle) -> bool {
        if (needle.empty())
            return false;
        return stack.find(needle, stack.size() - needle.size()) != std::string_view::npos;
    };

    if (ends_with(args.path_vectors, ".dbin"))
        bench_with_args<f64_t>(args);
    else if (ends_with(args.path_vectors, ".fbin") || ends_with(args.path_vectors, ".f32bin"))
        bench_with_args<f32_t>(args);
    else if (ends_with(args.path_vectors, ".hbin"))
        bench_with_args<f16_t>(args);
    else if (ends_with(args.path_vectors, ".i8bin"))
        bench_with_args<i8_t>(args);
    else if (ends_with(args.path_vectors, ".u8bin"))
        bench_with_args<u8_t>(args);
    else if (ends_with(args.path_vectors, ".bbin"))
        bench_with_args<b1x8_t>(args);
    else
        throw std::runtime_error("Unknown input file path");

    return 0;
}
