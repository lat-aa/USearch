/**
 *  @file       exec.hpp
 *  @brief      STL/OpenMP 执行器。
 */
#pragma once
// 执行器不依赖浮点位类型；与 floats 解耦以减小无关 TU 的包含图。
#include <plugins/kinds.hpp>
namespace unum {
namespace usearch {

/**
 *  @brief  An STL-based executor or a "thread-pool" for parallel execution.
 *          Isn't efficient for small batches, as it recreates the threads on every call.
 */
class executor_stl_t {
    std::size_t threads_count_{};

    struct jthread_t {
        std::thread native_;
        bool initialized_ = false;

        jthread_t() = default;
        jthread_t(jthread_t&&) = default;
        jthread_t(jthread_t const&) = delete;
        template <typename callable_at>
        jthread_t(callable_at&& func) : native_([=]() { func(); }), initialized_(true) {}

        ~jthread_t() {
            if (initialized_ && native_.joinable())
                native_.join();
        }
    };

  public:
    /**
     *  @param threads_count The number of threads to be used for parallel execution.
     */
    executor_stl_t(std::size_t threads_count = 0) noexcept
        : threads_count_(threads_count ? threads_count : std::thread::hardware_concurrency()) {}

    /**
     *  @return Maximum number of threads available to the executor.
     */
    std::size_t size() const noexcept { return threads_count_; }

    /**
     *  @brief Executes a fixed number of tasks using the specified thread-aware function.
     *  @param tasks                 The total number of tasks to be executed.
     *  @param thread_aware_function The thread-aware function to be called for each thread index and task index.
     *  @throws If an exception occurs during execution of the thread-aware function.
     */
    template <typename thread_aware_function_at>
    void fixed(std::size_t tasks, thread_aware_function_at&& thread_aware_function) noexcept(false) {
        buffer_gt<jthread_t> threads_pool(threads_count_ - 1); // Allocate space for threads minus the main thread
        std::size_t tasks_per_thread = tasks;
        std::size_t threads_count = (std::min)(threads_count_, tasks);
        if (threads_count > 1) {
            tasks_per_thread = (tasks / threads_count) + ((tasks % threads_count) != 0);
            for (std::size_t thread_idx = 1; thread_idx < threads_count; ++thread_idx) {
                new (&threads_pool[thread_idx - 1]) jthread_t([=]() {
#if USEARCH_USE_NUMKONG
                    nk_configure_thread_(nk_capabilities());
#endif
                    for (std::size_t task_idx = thread_idx * tasks_per_thread;
                         task_idx < (std::min)(tasks, thread_idx * tasks_per_thread + tasks_per_thread); ++task_idx)
                        thread_aware_function(thread_idx, task_idx);
                });
            }
        }
        for (std::size_t task_idx = 0; task_idx < (std::min)(tasks, tasks_per_thread); ++task_idx)
            thread_aware_function(0, task_idx);
    }

    /**
     *  @brief Executes limited number of tasks using the specified thread-aware function.
     *  @param tasks                 The upper bound on the number of tasks.
     *  @param thread_aware_function The thread-aware function to be called for each thread index and task index.
     *  @throws If an exception occurs during execution of the thread-aware function.
     */
    template <typename thread_aware_function_at>
    void dynamic(std::size_t tasks, thread_aware_function_at&& thread_aware_function) noexcept(false) {
        buffer_gt<jthread_t> threads_pool(threads_count_ - 1);
        std::size_t tasks_per_thread = tasks;
        std::size_t threads_count = (std::min)(threads_count_, tasks);
        std::atomic_bool stop{false};
        if (threads_count > 1) {
            tasks_per_thread = (tasks / threads_count) + ((tasks % threads_count) != 0);
            for (std::size_t thread_idx = 1; thread_idx < threads_count; ++thread_idx) {
                new (&threads_pool[thread_idx - 1]) jthread_t([=, &stop]() {
#if USEARCH_USE_NUMKONG
                    nk_configure_thread_(nk_capabilities());
#endif
                    for (std::size_t task_idx = thread_idx * tasks_per_thread;
                         task_idx < (std::min)(tasks, thread_idx * tasks_per_thread + tasks_per_thread) &&
                         !stop.load(std::memory_order_relaxed);
                         ++task_idx)
                        if (!thread_aware_function(thread_idx, task_idx))
                            stop.store(true, std::memory_order_relaxed);
                });
            }
        }
        for (std::size_t task_idx = 0;
             task_idx < (std::min)(tasks, tasks_per_thread) && !stop.load(std::memory_order_relaxed); ++task_idx)
            if (!thread_aware_function(0, task_idx))
                stop.store(true, std::memory_order_relaxed);
    }

    /**
     *  @brief Saturates every available thread with the given workload, until they finish.
     *  @param thread_aware_function The thread-aware function to be called for each thread index.
     *  @throws If an exception occurs during execution of the thread-aware function.
     */
    template <typename thread_aware_function_at>
    void parallel(thread_aware_function_at&& thread_aware_function) noexcept(false) {
        if (threads_count_ == 1)
            return thread_aware_function(0);
        buffer_gt<jthread_t> threads_pool(threads_count_ - 1);
        for (std::size_t thread_idx = 1; thread_idx < threads_count_; ++thread_idx)
            new (&threads_pool[thread_idx - 1]) jthread_t([=]() {
#if USEARCH_USE_NUMKONG
                nk_configure_thread_(nk_capabilities());
#endif
                thread_aware_function(thread_idx);
            });
        thread_aware_function(0);
    }
};

#if USEARCH_USE_OPENMP

/**
 *  @brief  An OpenMP-based executor or a "thread-pool" for parallel execution.
 *          Is the preferred implementation, when available, and target environment has OpenMP.
 */
class executor_openmp_t {
  public:
    /**
     *  @param threads_count The number of threads to be used for parallel execution.
     */
    executor_openmp_t(std::size_t threads_count = 0) noexcept {
        omp_set_num_threads(static_cast<int>(threads_count ? threads_count : std::thread::hardware_concurrency()));
#if USEARCH_USE_NUMKONG
        nk_capability_t caps = nk_capabilities();
        nk_configure_thread(caps);
#pragma omp parallel
        {
            nk_configure_thread(caps);
        }
#endif
    }

    /**
     *  @return Maximum number of threads available to the executor.
     */
    std::size_t size() const noexcept { return omp_get_max_threads(); }

    /**
     *  @brief Executes tasks in bulk using the specified thread-aware function.
     *  @param tasks                 The total number of tasks to be executed.
     *  @param thread_aware_function The thread-aware function to be called for each thread index and task index.
     *  @throws If an exception occurs during execution of the thread-aware function.
     */
    template <typename thread_aware_function_at>
    void fixed(std::size_t tasks, thread_aware_function_at&& thread_aware_function) noexcept(false) {
#pragma omp parallel for schedule(dynamic, 1)
        for (std::size_t i = 0; i != tasks; ++i) {
            thread_aware_function(omp_get_thread_num(), i);
        }
    }

    /**
     *  @brief Executes tasks in bulk using the specified thread-aware function.
     *  @param tasks                 The total number of tasks to be executed.
     *  @param thread_aware_function The thread-aware function to be called for each thread index and task index.
     *  @throws If an exception occurs during execution of the thread-aware function.
     */
    template <typename thread_aware_function_at>
    void dynamic(std::size_t tasks, thread_aware_function_at&& thread_aware_function) noexcept(false) {
        // OpenMP cancellation points are not yet available on most platforms, and require
        // the `OMP_CANCELLATION` environment variable to be set.
        // http://jakascorner.com/blog/2016/08/omp-cancel.html
        // if (omp_get_cancellation()) {
        // #pragma omp parallel for schedule(dynamic, 1)
        //     for (std::size_t i = 0; i != tasks; ++i) {
        // #pragma omp cancellation point for
        //         if (!thread_aware_function(omp_get_thread_num(), i)) {
        // #pragma omp cancel for
        //         }
        //     }
        // }
        std::atomic_bool stop{false};
#pragma omp parallel for schedule(dynamic, 1) shared(stop)
        for (std::size_t i = 0; i != tasks; ++i) {
            if (!stop.load(std::memory_order_relaxed) && !thread_aware_function(omp_get_thread_num(), i))
                stop.store(true, std::memory_order_relaxed);
        }
    }

    /**
     *  @brief Saturates every available thread with the given workload, until they finish.
     *  @param thread_aware_function The thread-aware function to be called for each thread index.
     *  @throws If an exception occurs during execution of the thread-aware function.
     */
    template <typename thread_aware_function_at>
    void parallel(thread_aware_function_at&& thread_aware_function) noexcept(false) {
#pragma omp parallel
        {
            thread_aware_function(omp_get_thread_num());
        }
    }
};

using executor_default_t = executor_openmp_t;

#else

using executor_default_t = executor_stl_t;

#endif

} // namespace usearch
} // namespace unum
