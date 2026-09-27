/**
 *  @file       exact.hpp
 *  @brief      exact_search、kmeans 与哈希表余下部分。
 */
#pragma once
#include <plugins/punned.hpp>
namespace unum {
namespace usearch {

/**
 *  @brief  View over a potentially-strided memory buffer, containing a row-major matrix.
 */
template <typename scalar_at> //
class matrix_slice_gt {
    using scalar_t = scalar_at;
    using byte_addressable_t = typename std::conditional<std::is_const<scalar_t>::value, byte_t const, byte_t>::type;

    scalar_t* begin_{};
    std::size_t dimensions_{};
    std::size_t count_{};
    std::size_t stride_bytes_{};

  public:
    matrix_slice_gt() noexcept = default;
    matrix_slice_gt(matrix_slice_gt const&) noexcept = default;
    matrix_slice_gt& operator=(matrix_slice_gt const&) noexcept = default;

    matrix_slice_gt(scalar_t* begin, std::size_t dimensions, std::size_t count = 1) noexcept
        : matrix_slice_gt(begin, dimensions, count, dimensions * sizeof(scalar_at)) {}

    matrix_slice_gt(scalar_t* begin, std::size_t dimensions, std::size_t count, std::size_t stride_bytes) noexcept
        : begin_(begin), dimensions_(dimensions), count_(count), stride_bytes_(stride_bytes) {}

    explicit operator bool() const noexcept { return begin_; }
    std::size_t size() const noexcept { return count_; }
    std::size_t dimensions() const noexcept { return dimensions_; }
    std::size_t stride_bytes() const noexcept { return stride_bytes_; }
    scalar_t* data() const noexcept { return begin_; }
    scalar_t* at(std::size_t i) const noexcept {
        return reinterpret_cast<scalar_t*>(reinterpret_cast<byte_addressable_t*>(begin_) + i * stride_bytes_);
    }
};

struct exact_offset_and_distance_t {
    u32_t offset;
    f32_t distance;
};

using exact_search_results_t = matrix_slice_gt<exact_offset_and_distance_t const>;

/**
 *  @brief  Helper-structure for exact search operations.
 *          Perfect if you have @b <1M vectors and @b <100 queries per call.
 *
 *  Uses a 3-step procedure to minimize:
 *  - cache-misses on vector lookups,
 *  - multi-threaded contention on concurrent writes.
 */
class exact_search_t {

    inline static bool smaller_distance(exact_offset_and_distance_t a, exact_offset_and_distance_t b) noexcept {
        return a.distance < b.distance;
    }

    using keys_and_distances_t = buffer_gt<exact_offset_and_distance_t>;
    keys_and_distances_t keys_and_distances;

  public:
    template <typename scalar_at, typename executor_at = dummy_executor_t, typename progress_at = dummy_progress_t>
    exact_search_results_t operator()(                                                      //
        matrix_slice_gt<scalar_at const> dataset, matrix_slice_gt<scalar_at const> queries, //
        std::size_t wanted, metric_punned_t const& metric,                                  //
        executor_at&& executor = executor_at{}, progress_at&& progress = progress_at{}) {
        return operator()(                                                                           //
            reinterpret_cast<byte_t const*>(dataset.data()), dataset.size(), dataset.stride_bytes(), //
            reinterpret_cast<byte_t const*>(queries.data()), queries.size(), queries.stride_bytes(), //
            wanted, metric, executor, progress);
    }

    template <typename executor_at = dummy_executor_t, typename progress_at = dummy_progress_t>
    exact_search_results_t operator()(                                                     //
        byte_t const* dataset_data, std::size_t dataset_count, std::size_t dataset_stride, //
        byte_t const* queries_data, std::size_t queries_count, std::size_t queries_stride, //
        std::size_t wanted, metric_punned_t const& metric, executor_at&& executor = executor_at{},
        progress_at&& progress = progress_at{}) {

        // Allocate temporary memory to store the distance matrix.
        // We keep two buffers - original and transposed, as in-place transpositions
        // of non-rectangular matrixes is expensive.
        std::size_t tasks_count = dataset_count * queries_count;
        if (keys_and_distances.size() < tasks_count * 2)
            keys_and_distances = keys_and_distances_t(tasks_count * 2);
        if (keys_and_distances.size() < tasks_count * 2)
            return {};

        exact_offset_and_distance_t* keys_and_distances_per_dataset = keys_and_distances.data();
        exact_offset_and_distance_t* keys_and_distances_per_query = keys_and_distances_per_dataset + tasks_count;

        // §1. Compute distances in a data-parallel fashion
        std::atomic<std::size_t> processed{0};
        executor.dynamic(dataset_count, [&](std::size_t thread_idx, std::size_t dataset_idx) {
            byte_t const* dataset = dataset_data + dataset_idx * dataset_stride;
            for (std::size_t query_idx = 0; query_idx != queries_count; ++query_idx) {
                byte_t const* query = queries_data + query_idx * queries_stride;
                auto distance = metric(dataset, query);
                std::size_t task_idx = queries_count * dataset_idx + query_idx;
                keys_and_distances_per_dataset[task_idx].offset = static_cast<u32_t>(dataset_idx);
                keys_and_distances_per_dataset[task_idx].distance = static_cast<f32_t>(distance);
            }

            // It's more efficient in this case to report progress from a single thread
            processed += queries_count;
            if (thread_idx == 0)
                if (!progress(processed.load(), tasks_count))
                    return false;
            return true;
        });
        if (processed.load() != tasks_count)
            return {};

        // §2. Transpose in a single thread to avoid contention writing into the same memory buffers
        for (std::size_t query_idx = 0; query_idx != queries_count; ++query_idx) {
            for (std::size_t dataset_idx = 0; dataset_idx != dataset_count; ++dataset_idx) {
                std::size_t from_idx = queries_count * dataset_idx + query_idx;
                std::size_t to_idx = dataset_count * query_idx + dataset_idx;
                keys_and_distances_per_query[to_idx] = keys_and_distances_per_dataset[from_idx];
            }
        }

        // §3. Partial-sort every query result
        executor.fixed(queries_count, [&](std::size_t, std::size_t query_idx) {
            auto start = keys_and_distances_per_query + dataset_count * query_idx;
            if (wanted > 1) {
                std::partial_sort(start, start + wanted, start + dataset_count, &smaller_distance);
            } else {
                auto min_it = std::min_element(start, start + dataset_count, &smaller_distance);
                if (min_it != start)
                    std::swap(*min_it, *start);
            }
        });

        // At the end report the latest numbers, because the reporter thread may be finished earlier
        progress(tasks_count, tasks_count);
        return {keys_and_distances_per_query, wanted, queries_count,
                dataset_count * sizeof(exact_offset_and_distance_t)};
    }
};

struct kmeans_clustering_result_t {
    error_t error{};
    std::size_t computed_distances{};
    /// @brief The number of iterations the algorithm took to converge.
    std::size_t iterations{};
    /// @brief The number of points that changed clusters in the last iteration.
    std::size_t last_iteration_points_shifted{};
    /// @brief The inertia of the last iteration (sum of squared distances to centroids).
    f64_t last_iteration_inertia{};
    /// @brief The total elapsed runtime of the algorithm in seconds.
    f64_t runtime_seconds{};
    /// @brief The total distance between the points and their assigned centroids.
    f64_t aggregate_distance{};

    explicit operator bool() const noexcept { return !error; }
    kmeans_clustering_result_t failed(error_t message) noexcept {
        error = std::move(message);
        return std::move(*this);
    }
};

/**
 *  @brief  Helper-class for K-Means clustering of dense vectors.
 *          Doesn't require constructing the index, but benefits from mixed-precision logic.
 *          ! Doesn't guarantee that the clusters are balanced in size.
 *
 *  The algorithm is as follows:
 *  - Initialization: Select K initial centroids (randomly or with a heuristic).
 *  - Assignment: Assign each data point to the nearest centroid based on the Euclidean distance.
 *  - Update: Recalculate the centroids as the mean of all points assigned to each centroid.
 *  - Repeat: Repeat the assignment and update steps until the centroids no longer change significantly
 *            or an early-exit condition is met.
 */
template <typename allocator_at = std::allocator<char>> class kmeans_clustering_gt {
  public:
    using distance_t = distance_punned_t;

    metric_kind_t metric_kind{metric_kind_t::l2sq_k};
    scalar_kind_t quantization_kind{scalar_kind_t::bf16_k};

    static constexpr std::size_t max_iterations_default_k = 300;
    static constexpr f64_t inertia_threshold_default_k = 1e-4;
    static constexpr f64_t max_seconds_default_k = 60.0;
    static constexpr f64_t min_shifts_default_k = 0.01;

    /// @brief Early-exit parameter - the maximum number of iterations to perform.
    std::size_t max_iterations{max_iterations_default_k};
    /// @brief Early-exit parameter - the threshold for the final inertia to terminate early.
    f64_t inertia_threshold{inertia_threshold_default_k};
    /// @brief Early-exit parameter - the maximum runtime allowed in seconds.
    f64_t max_seconds{max_seconds_default_k};
    /// @brief Early-exit parameter - the minimum share of points that must change clusters per iteration.
    f64_t min_shifts{min_shifts_default_k};
    /// @brief The random seed to use for centroid initialization.
    std::uint64_t seed{0};

    kmeans_clustering_gt(std::uint64_t seed) noexcept : seed(seed) {}
    kmeans_clustering_gt() noexcept(false) {
        std::random_device random_device;
        seed = random_device();
    }

    kmeans_clustering_gt(kmeans_clustering_gt const&) = default;
    kmeans_clustering_gt& operator=(kmeans_clustering_gt const&) = default;

    template <typename scalar_at, typename executor_at = dummy_executor_t, typename progress_at = dummy_progress_t>
    kmeans_clustering_result_t operator()( //
        matrix_slice_gt<scalar_at const> points, matrix_slice_gt<scalar_at> centroids,
        span_gt<std::size_t> point_to_centroid_index, span_gt<distance_t> point_to_centroid_distance, //
        executor_at&& executor = executor_at{}, progress_at&& progress = progress_at{}) {
        return operator()(                                                                        //
            reinterpret_cast<byte_t const*>(points.data()), points.size(), points.stride_bytes(), //
            reinterpret_cast<byte_t*>(centroids.data()), centroids.size(), centroids.stride_bytes(),
            point_to_centroid_index.data(), point_to_centroid_distance.data(), //
            scalar_kind<scalar_at>(), points.dimensions(), executor, progress);
    }

    template <typename executor_at = dummy_executor_t, typename progress_at = dummy_progress_t>
    kmeans_clustering_result_t operator()(                                                       //
        byte_t const* points_data, std::size_t points_count, std::size_t points_stride_bytes,    //
        byte_t* centroids_data, std::size_t wanted_clusters, std::size_t centroids_stride_bytes, //
        std::size_t* point_to_centroid_index, distance_t* point_to_centroid_distance,            //
        scalar_kind_t original_scalar_kind, std::size_t dimensions, executor_at&& executor = executor_at{},
        progress_at&& progress = progress_at{}) {

        (void)progress; // TODO

        // Perform sanity checks for algorithm settings.
        kmeans_clustering_result_t result;
        if (max_iterations < 1)
            return result.failed("The number of iterations must be at least 1");

        // Perform sanity checks for input arguments.
        if (wanted_clusters < 2)
            return result.failed("The number of clusters must be at least 2");
        if (wanted_clusters >= points_count)
            return result.failed("The number of clusters must be less than the number of vectors");

        metric_punned_t metric = metric_punned_t::builtin(dimensions, metric_kind, quantization_kind);
        if (!metric)
            return result.failed("Unsupported metric or scalar kind");

        // Let's allocate memory for the centroids coordinates and make sure it's
        // rows are aligned to cache lines to avoid false sharing.
        buffer_gt<distance_t, aligned_allocator_gt<distance_t, 64>> point_to_centroid_distance_buffer(points_count);
        buffer_gt<std::size_t, aligned_allocator_gt<std::size_t, 64>> point_to_centroid_index_buffer(points_count);
        buffer_gt<std::atomic<std::size_t>, aligned_allocator_gt<std::atomic<std::size_t>, 64>> cluster_sizes_buffer(
            wanted_clusters);

        // For a mixed precision computation, we keep the centroids represented in two forms -
        // double precision and quantized the same way as in the index, to avoid paying conversion penalties.
        // Double precision is needed to avoid accumulating errors when aggregating too many entries.
        std::size_t const bytes_per_vector_original =
            divide_round_up<CHAR_BIT>(dimensions * bits_per_scalar(original_scalar_kind));
        std::size_t const bytes_per_vector_quantized = metric.bytes_per_vector();
        std::size_t const stride_per_vector_quantized = divide_round_up<64>(bytes_per_vector_quantized) * 64;
        buffer_gt<byte_t, aligned_allocator_gt<byte_t, 64>> points_quantized_buffer( //
            points_count * stride_per_vector_quantized);
        buffer_gt<byte_t, aligned_allocator_gt<byte_t, 64>> centroids_quantized_buffer( //
            wanted_clusters * stride_per_vector_quantized);

        // When aggregating centroids, we want to parallelize the operation and need more memory.
        // For every thread we keep two double-precision vectors. One is the up-casting output buffer for quantized
        // coordinates, and the other is the temporary buffer for the partial sums of the double-precision coordinates.
        // The ordering:
        //
        //      - thread 0: [centroid 0, centroid 1, centroid 2, centroid 3, ...]
        //      - thread 1: [centroid 0, centroid 1, centroid 2, centroid 3, ...]
        //      - thread 2: [centroid 0, centroid 1, centroid 2, centroid 3, ...]
        //
        std::size_t const thread_count = executor.size();
        buffer_gt<f64_t, aligned_allocator_gt<f64_t, 64>> centroids_precise_buffer( //
            wanted_clusters * dimensions * thread_count);
        buffer_gt<f64_t, aligned_allocator_gt<f64_t, 64>> points_precise_buffer( //
            wanted_clusters * dimensions * thread_count);

        // Check if all memory allocations were successful.
        if (!centroids_precise_buffer || !points_precise_buffer || !centroids_quantized_buffer ||
            !point_to_centroid_index_buffer || !cluster_sizes_buffer || !point_to_centroid_distance_buffer ||
            !points_quantized_buffer)
            return result.failed("No memory for result outputs!");

        std::fill_n(point_to_centroid_index_buffer.data(), points_count, wanted_clusters);
        std::fill_n(point_to_centroid_distance_buffer.data(), points_count, (std::numeric_limits<distance_t>::max)());

        // Initialize the casting kernel for quantization and export.
        casts_punned_t casts = casts_punned_t::make(quantization_kind);
        cast_punned_t const& compress_points = casts.from[original_scalar_kind];
        cast_punned_t const& decompress_points = casts.to[original_scalar_kind];
        cast_punned_t const& compress_precise = casts.from.f64;
        cast_punned_t const& decompress_precise = casts.to.f64;
        for (std::size_t i = 0; i < points_count; i++) {
            byte_t const* vector = points_data + i * points_stride_bytes;
            byte_t* quantized = points_quantized_buffer.data() + i * stride_per_vector_quantized;
            if (!compress_points(vector, dimensions, quantized))
                std::memcpy(quantized, vector, bytes_per_vector_original);
        }

        // Initialize centroids with random points vectors.
        std::mt19937_64 random_engine;
        random_engine.seed(seed);
        for (std::size_t i = 0; i < wanted_clusters; i++) {
            // Generate the random index of the points vector,
            // that is unique and not already used as a centroid.
            std::size_t random_index;
            do {
                random_index = random_engine() % points_count;
                bool is_unique = true;
                for (std::size_t j = 0; j < i; j++) {
                    if (point_to_centroid_index_buffer[j] == random_index) {
                        is_unique = false;
                        break;
                    }
                }
                if (is_unique)
                    break;
            } while (true);

            // Copy the vector to the centroid and quantize it.
            byte_t const* quantized_point = points_quantized_buffer.data() + random_index * stride_per_vector_quantized;
            byte_t* quantized_centroid = centroids_quantized_buffer.data() + i * stride_per_vector_quantized;
            std::memcpy(quantized_centroid, quantized_point, bytes_per_vector_quantized);
            point_to_centroid_index_buffer[random_index] = i;
            point_to_centroid_distance_buffer[random_index] = 0;
        }

        auto start_time = std::chrono::high_resolution_clock::now();
        std::size_t iterations = 0;
        std::size_t const min_points_shifted_per_iteration = static_cast<std::size_t>(min_shifts * points_count);
        f64_t last_aggregate_distance = (std::numeric_limits<f64_t>::max)();

        while (iterations < max_iterations) {
            iterations++;

            // For every point, find the closest centroid.
            std::atomic<std::size_t> points_shifted{0};
            executor.dynamic(points_count, [&](std::size_t, std::size_t points_idx) {
                byte_t const* quantized_point =
                    points_quantized_buffer.data() + points_idx * stride_per_vector_quantized;
                byte_t const* quantized_centroids = centroids_quantized_buffer.data();
                distance_t closest_distance_local = (std::numeric_limits<distance_t>::max)();
                std::size_t closest_idx_local = 0;
                for (std::size_t centroid_idx = 0; centroid_idx < wanted_clusters; centroid_idx++) {
                    byte_t const* quantized_centroid = quantized_centroids + centroid_idx * stride_per_vector_quantized;
                    distance_t distance = metric(quantized_point, quantized_centroid);
                    if (distance < closest_distance_local) {
                        closest_distance_local = distance;
                        closest_idx_local = centroid_idx;
                    }
                }

                distance_t& closest_distance_ref = point_to_centroid_distance_buffer[points_idx];
                std::size_t& closest_idx_ref = point_to_centroid_index_buffer[points_idx];
                if (closest_idx_local != closest_idx_ref) {
                    closest_idx_ref = closest_idx_local;
                    points_shifted.fetch_add(1, std::memory_order_relaxed);
                }

                closest_distance_ref = closest_distance_local;
                return true;
            });

            f64_t aggregate_distance = 0.0;
            for (std::size_t i = 0; i < points_count; i++)
                aggregate_distance += point_to_centroid_distance_buffer[i];
            f64_t aggregate_distance_change =
                std::abs(aggregate_distance - last_aggregate_distance) / last_aggregate_distance;

            auto current_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<f64_t> elapsed_time = current_time - start_time;
            result.runtime_seconds = elapsed_time.count();
            result.last_iteration_inertia = aggregate_distance_change;
            result.last_iteration_points_shifted = points_shifted.load(std::memory_order_relaxed);

            // Check for early-exit conditions
            if (last_aggregate_distance != 0.0 && inertia_threshold != 0.0)
                if (aggregate_distance_change <= inertia_threshold)
                    break;
            if (min_points_shifted_per_iteration != 0 || result.last_iteration_points_shifted == 0)
                if (result.last_iteration_points_shifted <= min_points_shifted_per_iteration)
                    break;
            if (max_seconds != 0)
                if (result.runtime_seconds >= max_seconds)
                    break;

            // For every centroid, recalculate the mean of all points assigned to it.
            // That part is problematic to parallelize on many-core-systems, because of the contention.
            // Alternatively, a tree-like approach can be used, where every core accumulates it's own partial sums.
            // And those are later aggregated by a single thread.
            std::memset(centroids_precise_buffer.data(), 0,
                        wanted_clusters * dimensions * thread_count * sizeof(f64_t));
            std::memset(reinterpret_cast<byte_t*>(cluster_sizes_buffer.data()), 0,
                        wanted_clusters * sizeof(std::atomic<std::size_t>));
            executor.dynamic(points_count, [&](std::size_t thread_idx, std::size_t points_idx) {
                std::size_t centroid_idx = point_to_centroid_index_buffer[points_idx];
                byte_t const* quantized_point =
                    points_quantized_buffer.data() + points_idx * stride_per_vector_quantized;
                f64_t* centroid_precise = centroids_precise_buffer.data() + wanted_clusters * dimensions * thread_idx +
                                          centroid_idx * dimensions;

                // Upcast the points point into a buffer of double-precision floats.
                f64_t* point_precise = points_precise_buffer.data() + wanted_clusters * dimensions * thread_idx +
                                       centroid_idx * dimensions;
                if (!decompress_precise(quantized_point, dimensions, reinterpret_cast<byte_t*>(point_precise)))
                    std::memcpy(reinterpret_cast<byte_t*>(point_precise), quantized_point, bytes_per_vector_quantized);

                // Now add the vector from the points into the centroid partial sum.
                for (std::size_t i = 0; i < dimensions; i++)
                    centroid_precise[i] += point_precise[i];

                cluster_sizes_buffer[centroid_idx].fetch_add(1, std::memory_order_relaxed);
                return true;
            });

            // Aggregate the partial sums into the final centroids - storing them in the high-precision
            // buffer of the first thread. Normalization procedure is different for different metrics.
            for (std::size_t centroid_idx = 0; centroid_idx < wanted_clusters; centroid_idx++) {
                f64_t* centroid_precise_aggregated = centroids_precise_buffer.data() + centroid_idx * dimensions;
                for (std::size_t thread_idx = 1; thread_idx < thread_count; thread_idx++) {
                    f64_t* centroid_precise = centroids_precise_buffer.data() +
                                              wanted_clusters * dimensions * thread_idx + centroid_idx * dimensions;
                    for (std::size_t i = 0; i < dimensions; i++)
                        centroid_precise_aggregated[i] += centroid_precise[i];
                }

                // Normalize based on the metric kind
                if (metric_kind == metric_kind_t::l2sq_k) {
                    // Normalize for Euclidean distance (L2)
                    std::size_t cluster_size = cluster_sizes_buffer[centroid_idx].load(std::memory_order_relaxed);
                    if (cluster_size > 0)
                        for (std::size_t i = 0; i < dimensions; i++)
                            centroid_precise_aggregated[i] /= static_cast<f64_t>(cluster_size);

                } else if (metric_kind == metric_kind_t::cos_k) {
                    // Normalize for Cosine distance
                    f64_t norm = 0.0;
                    for (std::size_t i = 0; i < dimensions; i++)
                        norm += centroid_precise_aggregated[i] * centroid_precise_aggregated[i];
                    norm = std::sqrt(norm);
                    if (norm > 0.0)
                        for (std::size_t i = 0; i < dimensions; i++)
                            centroid_precise_aggregated[i] /= norm;
                }

                // Quantize the centroid after normalization for further iterations
                byte_t* centroid_quantized =
                    centroids_quantized_buffer.data() + centroid_idx * stride_per_vector_quantized;
                if (!compress_precise(reinterpret_cast<byte_t*>(centroid_precise_aggregated), dimensions,
                                      centroid_quantized))
                    std::memcpy(centroid_quantized, reinterpret_cast<byte_t*>(centroid_precise_aggregated),
                                bytes_per_vector_quantized);
            }
        }

        // Export stats.
        result.iterations = iterations;
        result.computed_distances = points_count * wanted_clusters * iterations;
        result.aggregate_distance = 0;
        for (distance_t distance : point_to_centroid_distance_buffer)
            result.aggregate_distance += distance;

        // We've finished all the iterations, now we can export the centroids back to the original precision.
        std::memcpy(point_to_centroid_index, point_to_centroid_index_buffer.data(), points_count * sizeof(std::size_t));
        std::memcpy(point_to_centroid_distance, point_to_centroid_distance_buffer.data(),
                    points_count * sizeof(distance_t));
        for (std::size_t i = 0; i < wanted_clusters; i++) {
            byte_t const* quantized_centroid = centroids_quantized_buffer.data() + i * stride_per_vector_quantized;
            byte_t* centroid = centroids_data + i * centroids_stride_bytes;
            if (!decompress_points(quantized_centroid, dimensions, centroid))
                std::memcpy(centroid, quantized_centroid, bytes_per_vector_quantized);
        }

        return result;
    }
};

using kmeans_clustering_t = kmeans_clustering_gt<>;

/**
 *  @brief  C++11 Multi-Hash-Set with Linear Probing.
 *
 *  - Allows multiple equivalent values,
 *  - Supports transparent hashing and equality operator.
 *  - Doesn't throw exceptions, if forbidden.
 *  - Doesn't need reserving a value for deletions.
 *
 *  @section Layout
 *
 *  For every slot we store 2 extra bits for 3 possible states: empty, populated, or deleted.
 *  With linear probing the hashes at the end of the populated region will spill into its first half.
 */
template <typename element_at, typename hash_at, typename equals_at, typename allocator_at = std::allocator<char>>
class flat_hash_multi_set_gt {
  public:
    using element_t = element_at;
    using hash_t = hash_at;
    using equals_t = equals_at;
    using allocator_t = allocator_at;

    static constexpr std::size_t slots_per_bucket() { return 64; }
    static constexpr std::size_t bytes_per_bucket() {
        return slots_per_bucket() * sizeof(element_t) + sizeof(bucket_header_t);
    }

  private:
    struct bucket_header_t {
        std::uint64_t populated{};
        std::uint64_t deleted{};
    };
    char* data_ = nullptr;
    std::size_t buckets_ = 0;
    std::size_t populated_slots_ = 0;
    /// @brief  Number of tombstones (slots marked deleted but not yet reclaimed)
    std::size_t deleted_slots_ = 0;
    /// @brief  Number of slots
    std::size_t capacity_slots_ = 0;

    struct slot_ref_t {
        bucket_header_t& header;
        std::uint64_t mask;
        element_t& element;
    };

    slot_ref_t slot_ref(char* data, std::size_t slot_index) const noexcept {
        std::size_t bucket_index = slot_index / slots_per_bucket();
        std::size_t in_bucket_index = slot_index % slots_per_bucket();
        auto bucket_pointer = data + bytes_per_bucket() * bucket_index;
        auto slot_pointer = bucket_pointer + sizeof(bucket_header_t) + sizeof(element_t) * in_bucket_index;
        return {
            *reinterpret_cast<bucket_header_t*>(bucket_pointer),
            static_cast<std::uint64_t>(1ull) << in_bucket_index,
            *reinterpret_cast<element_t*>(slot_pointer),
        };
    }

    slot_ref_t slot_ref(std::size_t slot_index) const noexcept { return slot_ref(data_, slot_index); }

    bool populate_slot(slot_ref_t slot, element_t const& new_element) {
        if (slot.header.populated & slot.mask) {
            slot.element = new_element;
            slot.header.deleted &= ~slot.mask;
            return false;
        } else {
            new (&slot.element) element_t(new_element);
            slot.header.populated |= slot.mask;
            return true;
        }
    }

    /**
     *  @brief  Copies every live entry of @p source into the freshly zeroed @p target.
     *
     *  Tombstones are never carried over, so probe chains must be rebuilt from the hash
     *  rather than reproduced slot-for-slot: a live entry displaced past a tombstone by
     *  linear probing would otherwise become unreachable once that tombstone reads empty.
     *  The target holds no tombstones, so an unpopulated slot is always a free slot.
     */
    void rehash_into(char* source, std::size_t source_slots, char* target, std::size_t target_slots) const noexcept {
        hash_t hasher;
        for (std::size_t i = 0; i != source_slots; ++i) {
            slot_ref_t source_slot = slot_ref(source, i);
            if (!(source_slot.header.populated & source_slot.mask) || (source_slot.header.deleted & source_slot.mask))
                continue;

            std::size_t target_index = hasher(source_slot.element) & (target_slots - 1);
            while (true) {
                slot_ref_t target_slot = slot_ref(target, target_index);
                if (!(target_slot.header.populated & target_slot.mask)) {
                    new (&target_slot.element) element_t(source_slot.element);
                    target_slot.header.populated |= target_slot.mask;
                    break;
                }
                target_index = (target_index + 1) & (target_slots - 1);
            }
        }
    }

  public:
    std::size_t size() const noexcept { return populated_slots_; }
    std::size_t capacity() const noexcept { return capacity_slots_ * 2u / 3u; }
    std::size_t capacity_slots() const noexcept { return capacity_slots_; }

    flat_hash_multi_set_gt() noexcept {}
    ~flat_hash_multi_set_gt() noexcept { reset(); }

    flat_hash_multi_set_gt(flat_hash_multi_set_gt const& other) {

        // On Windows allocating a zero-size array would fail
        if (!other.buckets_) {
            reset();
            return;
        }

        // Allocate new memory
        checked_size_result_t bytes = checked_mul(other.buckets_, bytes_per_bucket());
        if (!bytes)
            usearch_raise_runtime_error("failed memory allocation");
        data_ = (char*)allocator_t{}.allocate(bytes.value);
        if (!data_)
            usearch_raise_runtime_error("failed memory allocation");

        // Copy metadata. Only live entries are rehashed below, so the copy has no tombstones.
        buckets_ = other.buckets_;
        populated_slots_ = other.populated_slots_;
        deleted_slots_ = 0;
        capacity_slots_ = other.capacity_slots_;

        // Initialize new buckets to empty
        std::memset(data_, 0, buckets_ * bytes_per_bucket());
        rehash_into(other.data_, other.capacity_slots_, data_, capacity_slots_);
    }

    flat_hash_multi_set_gt& operator=(flat_hash_multi_set_gt const& other) {

        // On Windows allocating a zero-size array would fail
        if (!other.buckets_) {
            reset();
            return *this;
        }

        // Handle self-assignment
        if (this == &other)
            return *this;

        // Clear existing data
        clear();
        if (data_)
            allocator_t{}.deallocate(data_, buckets_ * bytes_per_bucket());

        // Allocate new memory
        checked_size_result_t bytes = checked_mul(other.buckets_, bytes_per_bucket());
        if (!bytes)
            usearch_raise_runtime_error("failed memory allocation");
        data_ = (char*)allocator_t{}.allocate(bytes.value);
        if (!data_)
            usearch_raise_runtime_error("failed memory allocation");

        // Copy metadata. Only live entries are rehashed below, so the copy has no tombstones.
        buckets_ = other.buckets_;
        populated_slots_ = other.populated_slots_;
        deleted_slots_ = 0;
        capacity_slots_ = other.capacity_slots_;

        // Initialize new buckets to empty
        std::memset(data_, 0, buckets_ * bytes_per_bucket());
        rehash_into(other.data_, other.capacity_slots_, data_, capacity_slots_);

        return *this;
    }

    void clear() noexcept {
        // Call the destructors
        for (std::size_t i = 0; i < capacity_slots_; ++i) {
            slot_ref_t slot = slot_ref(i);
            if ((slot.header.populated & slot.mask) & (~slot.header.deleted & slot.mask))
                slot.element.~element_t();
        }

        // Reset populated slots count
        if (data_)
            std::memset(data_, 0, buckets_ * bytes_per_bucket());
        populated_slots_ = 0;
        deleted_slots_ = 0;
    }

    void reset() noexcept {
        clear(); // Clear all elements
        if (data_)
            allocator_t{}.deallocate(data_, buckets_ * bytes_per_bucket());
        data_ = nullptr;
        buckets_ = 0;
        populated_slots_ = 0;
        deleted_slots_ = 0;
        capacity_slots_ = 0;
    }

    /**
     *  @brief  Grows the table to fit @p capacity live entries, reclaiming tombstones.
     *
     *  Tombstones are reclaimed even when no growth is requested. They only ever stop
     *  being created once reclaimed, and a table with no empty slot left cannot
     *  terminate a probe, silently stranding live entries.
     */
    bool try_reserve(std::size_t capacity) noexcept {
        if (capacity <= this->capacity() && deleted_slots_ == 0)
            return true;

        // Calculate new sizes
        checked_size_result_t scaled_capacity = checked_mul(capacity, std::size_t{3});
        if (!scaled_capacity)
            return false;
        checked_size_result_t slots_needed = checked_divide_round_up(scaled_capacity.value, std::size_t{2});
        if (!slots_needed)
            return false;
        checked_size_result_t new_slots_checked = checked_ceil2(slots_needed.value);
        if (!new_slots_checked)
            return false;
        checked_size_result_t new_buckets_checked =
            checked_divide_round_up(new_slots_checked.value, slots_per_bucket());
        if (!new_buckets_checked)
            return false;
        checked_size_result_t new_slots = checked_mul(new_buckets_checked.value, slots_per_bucket());
        if (!new_slots)
            return false;

        // Never shrink: reclaiming tombstones alone needs no more than the current capacity
        std::size_t target_buckets = new_buckets_checked.value;
        std::size_t target_slots = new_slots.value;
        if (target_slots < capacity_slots_) {
            target_buckets = buckets_;
            target_slots = capacity_slots_;
        }
        checked_size_result_t new_bytes = checked_mul(target_buckets, bytes_per_bucket());
        if (!new_bytes)
            return false;

        // Allocate new memory
        char* new_data = (char*)allocator_t{}.allocate(new_bytes.value);
        if (!new_data)
            return false;

        // Initialize new buckets to empty
        std::memset(new_data, 0, new_bytes.value);
        rehash_into(data_, capacity_slots_, new_data, target_slots);

        // Deallocate old data and update pointers and sizes
        if (data_)
            allocator_t{}.deallocate(data_, buckets_ * bytes_per_bucket());
        data_ = new_data;
        buckets_ = target_buckets;
        capacity_slots_ = target_slots;
        deleted_slots_ = 0;

        return true;
    }

    template <typename query_at> class equal_iterator_gt {
      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = element_t;
        using difference_type = std::ptrdiff_t;
        using pointer = element_t*;
        using reference = element_t&;

        equal_iterator_gt(std::size_t index, flat_hash_multi_set_gt* parent, query_at const& query,
                          equals_t const& equals)
            : index_(index), parent_(parent), query_(query), equals_(equals) {}

        // Pre-increment: advance past tombstones and non-matching entries,
        // stopping at the next matching live entry or an empty slot. When every
        // slot is either live or tombstoned (no empty slot exists), the probe
        // saturates after `capacity_slots_` steps and the iterator becomes
        // `end()` - otherwise the loop would spin forever.
        equal_iterator_gt& operator++() {
            for (std::size_t remaining = parent_->capacity_slots_; remaining; --remaining) {
                index_ = (index_ + 1) & (parent_->capacity_slots_ - 1);
                auto slot = parent_->slot_ref(index_);
                bool is_empty = ~slot.header.populated & slot.mask;
                bool is_match = !(slot.header.deleted & slot.mask) && equals_(slot.element, query_);
                if (is_empty || is_match)
                    return *this;
            }
            index_ = parent_->capacity_slots_; // saturated probe -> end()
            return *this;
        }

        equal_iterator_gt operator++(int) {
            equal_iterator_gt temp = *this;
            ++(*this);
            return temp;
        }

        reference operator*() { return parent_->slot_ref(index_).element; }
        pointer operator->() { return &parent_->slot_ref(index_).element; }
        bool operator!=(equal_iterator_gt const& other) const { return !(*this == other); }
        bool operator==(equal_iterator_gt const& other) const {
            return index_ == other.index_ && parent_ == other.parent_;
        }

      private:
        std::size_t index_;
        flat_hash_multi_set_gt* parent_;
        query_at query_;  // Store the query object
        equals_t equals_; // Store the equals functor
    };

    /**
     *  @brief  Returns an iterator range of all elements matching the given query.
     *
     *  Technically, the second iterator points to the first empty slot after a
     *  range of equal values and non-equal values with similar hashes.
     */
    template <typename query_at>
    std::pair<equal_iterator_gt<query_at>, equal_iterator_gt<query_at>>
    equal_range(query_at const& query) const noexcept {

        equals_t equals;
        auto this_ptr = const_cast<flat_hash_multi_set_gt*>(this);
        auto end = equal_iterator_gt<query_at>(capacity_slots_, this_ptr, query, equals);
        if (!capacity_slots_)
            return {end, end};

        hash_t hasher;
        std::size_t hash_value = hasher(query);
        std::size_t first_equal_index = hash_value & (capacity_slots_ - 1);
        std::size_t const start_index = first_equal_index;

        // Linear probing to find the first equal element
        do {
            slot_ref_t slot = slot_ref(first_equal_index);
            if (slot.header.populated & ~slot.header.deleted & slot.mask) {
                if (equals(slot.element, query))
                    break;
            }
            // Stop if we find an empty slot
            else if (~slot.header.populated & slot.mask)
                return {end, end};

            // Move to the next slot
            first_equal_index = (first_equal_index + 1) & (capacity_slots_ - 1);
        } while (first_equal_index != start_index);

        // If no matching element was found, return end iterators
        if (first_equal_index == capacity_slots_)
            return {end, end};

        // Start from the first matching element and find the end of the populated range
        std::size_t first_empty_index = first_equal_index;
        do {
            first_empty_index = (first_empty_index + 1) & (capacity_slots_ - 1);
            slot_ref_t slot = slot_ref(first_empty_index);

            // If we find an empty slot, this is our end
            if (~slot.header.populated & slot.mask)
                break;
        } while (first_empty_index != start_index);

        return {equal_iterator_gt<query_at>(first_equal_index, this_ptr, query, equals),
                equal_iterator_gt<query_at>(first_empty_index, this_ptr, query, equals)};
    }

    template <typename similar_at> bool pop_first(similar_at&& query, element_t& popped_value) noexcept {

        if (!capacity_slots_)
            return false;

        hash_t hasher;
        equals_t equals;
        std::size_t hash_value = hasher(query);
        std::size_t slot_index = hash_value & (capacity_slots_ - 1); // Assuming capacity_slots_ is a power of 2
        std::size_t start_index = slot_index;                        // To detect loop in probing

        // Linear probing to find the first match
        do {
            slot_ref_t slot = slot_ref(slot_index);
            if (slot.header.populated & slot.mask) {
                if ((~slot.header.deleted & slot.mask) && equals(slot.element, query)) {
                    // Found a match, mark as deleted
                    slot.header.deleted |= slot.mask;
                    --populated_slots_;
                    ++deleted_slots_;
                    popped_value = slot.element;
                    return true; // Successfully removed
                }
            } else {
                // Stop if we find an empty slot
                break;
            }

            // Move to the next slot
            slot_index = (slot_index + 1) & (capacity_slots_ - 1); // Assuming capacity_slots_ is a power of 2
        } while (slot_index != start_index);

        return false; // No match found
    }

    template <typename similar_at> std::size_t erase(similar_at&& query) noexcept {

        if (!capacity_slots_)
            return 0;

        hash_t hasher;
        equals_t equals;
        std::size_t hash_value = hasher(query);
        std::size_t slot_index = hash_value & (capacity_slots_ - 1); // Assuming capacity_slots_ is a power of 2
        std::size_t const start_index = slot_index;                  // To detect loop in probing
        std::size_t count = 0;                                       // Count of elements removed

        // Linear probing to find all matches
        do {
            slot_ref_t slot = slot_ref(slot_index);
            if (slot.header.populated & slot.mask) {
                if ((~slot.header.deleted & slot.mask) && equals(slot.element, query)) {
                    // Found a match, mark as deleted
                    slot.header.deleted |= slot.mask;
                    --populated_slots_;
                    ++deleted_slots_;
                    ++count; // Increment count of elements removed
                }
            } else {
                // Stop if we find an empty slot
                break;
            }

            // Move to the next slot
            slot_index = (slot_index + 1) & (capacity_slots_ - 1); // Assuming capacity_slots_ is a power of 2
        } while (slot_index != start_index);

        return count; // Return the number of elements removed
    }

    template <typename similar_at> element_t const* find(similar_at&& query) const noexcept {

        if (!capacity_slots_)
            return nullptr;

        hash_t hasher;
        equals_t equals;
        std::size_t hash_value = hasher(query);
        std::size_t slot_index = hash_value & (capacity_slots_ - 1); // Assuming capacity_slots_ is a power of 2
        std::size_t start_index = slot_index;                        // To detect loop in probing

        // Linear probing to find the first match
        do {
            slot_ref_t slot = slot_ref(slot_index);
            if (slot.header.populated & slot.mask) {
                if ((~slot.header.deleted & slot.mask) && equals(slot.element, query))
                    return &slot.element; // Found a match, return pointer to the element
            } else {
                // Stop if we find an empty slot
                break;
            }

            // Move to the next slot
            slot_index = (slot_index + 1) & (capacity_slots_ - 1); // Assuming capacity_slots_ is a power of 2
        } while (slot_index != start_index);

        return nullptr; // No match found
    }

    element_t const* end() const noexcept { return nullptr; }

    template <typename func_at> void for_each(func_at&& func) const {
        for (std::size_t bucket_index = 0; bucket_index < buckets_; ++bucket_index) {
            auto bucket_pointer = data_ + bytes_per_bucket() * bucket_index;
            bucket_header_t& header = *reinterpret_cast<bucket_header_t*>(bucket_pointer);
            std::uint64_t populated = header.populated;
            std::uint64_t deleted = header.deleted;

            // Iterate through slots in the bucket
            for (std::size_t in_bucket_index = 0; in_bucket_index < slots_per_bucket(); ++in_bucket_index) {
                std::uint64_t mask = std::uint64_t(1ull) << in_bucket_index;

                // Check if the slot is populated and not deleted
                if ((populated & ~deleted) & mask) {
                    auto slot_pointer = bucket_pointer + sizeof(bucket_header_t) + sizeof(element_t) * in_bucket_index;
                    element_t const& element = *reinterpret_cast<element_t const*>(slot_pointer);
                    func(element);
                }
            }
        }
    }

    template <typename similar_at> std::size_t count(similar_at&& query) const noexcept {

        if (!capacity_slots_)
            return 0;

        hash_t hasher;
        equals_t equals;
        std::size_t hash_value = hasher(query);
        std::size_t slot_index = hash_value & (capacity_slots_ - 1);
        std::size_t start_index = slot_index; // To detect loop in probing
        std::size_t count = 0;

        // Linear probing to find the range
        do {
            slot_ref_t slot = slot_ref(slot_index);
            if ((slot.header.populated & slot.mask) && (~slot.header.deleted & slot.mask)) {
                if (equals(slot.element, query))
                    ++count;
            } else if (~slot.header.populated & slot.mask) {
                // Stop if we find an empty slot
                break;
            }

            // Move to the next slot
            slot_index = (slot_index + 1) & (capacity_slots_ - 1);
        } while (slot_index != start_index);

        return count;
    }

    template <typename similar_at> bool contains(similar_at&& query) const noexcept {

        if (!capacity_slots_)
            return false;

        hash_t hasher;
        equals_t equals;
        std::size_t hash_value = hasher(query);
        std::size_t slot_index = hash_value & (capacity_slots_ - 1);
        std::size_t start_index = slot_index; // To detect loop in probing

        // Linear probing to find the first match
        do {
            slot_ref_t slot = slot_ref(slot_index);
            if (slot.header.populated & slot.mask) {
                if ((~slot.header.deleted & slot.mask) && equals(slot.element, query))
                    return true; // Found a match, exit early
            } else
                // Stop if we find an empty slot
                break;

            // Move to the next slot
            slot_index = (slot_index + 1) & (capacity_slots_ - 1);
        } while (slot_index != start_index);

        return false; // No match found
    }

    void reserve(std::size_t capacity) {
        if (!try_reserve(capacity))
            usearch_raise_runtime_error("failed to reserve memory");
    }

    bool try_emplace(element_t const& element) noexcept {
        // Both live entries and tombstones consume slots a probe must walk past, so the
        // load factor counts them together. Under churn the live count alone stays flat
        // and would never trigger the rehash that reclaims the tombstones.
        if ((populated_slots_ + deleted_slots_) * 3u >= capacity_slots_ * 2u)
            if (!try_reserve(populated_slots_ + 1))
                return false;

        hash_t hasher;
        std::size_t hash_value = hasher(element);
        std::size_t slot_index = hash_value & (capacity_slots_ - 1);

        // Linear probing
        while (true) {
            slot_ref_t slot = slot_ref(slot_index);
            if ((~slot.header.populated & slot.mask) | (slot.header.deleted & slot.mask)) {
                // Found an empty or deleted slot; reusing a tombstone reclaims it.
                // Read the tombstone bit before `populate_slot` clears it.
                deleted_slots_ -= (slot.header.deleted & slot.mask) != 0;
                populate_slot(slot, element);
                ++populated_slots_;
                return true;
            }
            // Move to the next slot
            slot_index = (slot_index + 1) & (capacity_slots_ - 1);
        }
    }
};

} // namespace usearch
} // namespace unum
