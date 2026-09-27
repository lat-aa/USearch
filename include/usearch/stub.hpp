/**
 *  @file       stub.hpp
 *  @brief      谓词/进度/预取/执行器等可替换桩类型与类型特征。
 */
#pragma once
#include <usearch/config.hpp>
namespace unum {
namespace usearch {

/// @brief  C++17 and newer version deprecate the `std::result_of`
template <typename metric_at, typename... args_at>
using return_type_gt =
#if defined(USEARCH_DEFINED_CPP17)
    typename std::invoke_result<metric_at, args_at...>::type;
#else
    typename std::result_of<metric_at(args_at...)>::type;
#endif

/**
 *  @brief  An example of what a USearch-compatible ad-hoc filter would look like.
 *
 *  A similar function object can be passed to search queries to further filter entries
 *  on their auxiliary properties, such as some categorical keys stored in an external DBMS.
 */
struct dummy_predicate_t {
    template <typename member_at> constexpr bool operator()(member_at&&) const noexcept { return true; }
};

/**
 *  @brief  An example of what a USearch-compatible ad-hoc operation on in-flight entries.
 *
 *  This kind of callbacks is used when the engine is being updated and you want to patch
 *  the entries, while their are still under locks - limiting concurrent access and providing
 *  consistency.
 */
struct dummy_callback_t {
    template <typename member_at> void operator()(member_at&&) const noexcept {}
};

/**
 *  @brief  An example of what a USearch-compatible progress-bar should look like.
 *
 *  This is particularly helpful when handling long-running tasks, like serialization,
 *  saving, and loading from disk, or index-level joins.
 *  The reporter checks return value to continue or stop the process, `false` means need to stop.
 */
struct dummy_progress_t {
    inline bool operator()(std::size_t /*processed*/, std::size_t /*total*/) const noexcept { return true; }
};

/**
 *  @brief  An example of what a USearch-compatible values prefetching mechanism should look like.
 *
 *  USearch is designed to handle very large datasets, that may not fir into RAM. Fetching from
 *  external memory is very expensive, so we've added a pre-fetching mechanism, that accepts
 *  multiple objects at once, to cache in RAM ahead of the computation.
 *  The received iterators support both `get_slot` and `get_key` operations.
 *  An example usage may look like this:
 *
 *      template <typename member_citerator_like_at>
 *      inline void operator()(member_citerator_like_at, member_citerator_like_at) const noexcept {
 *          for (; begin != end; ++begin)
 *              io_uring_prefetch(offset_in_file(get_key(begin)));
 *      }
 */
struct dummy_prefetch_t {
    template <typename member_citerator_like_at>
    inline void operator()(member_citerator_like_at, member_citerator_like_at) const noexcept {}
};

/**
 *  @brief  An example of what a USearch-compatible executor (thread-pool) should look like.
 *
 *  It's expected to have `parallel(callback)` API to schedule one task per thread;
 *  an identical `fixed(count, callback)` and `dynamic(count, callback)` overloads that also accepts
 *  the number of tasks, and somehow schedules them between threads; as well as `size()` to
 *  determine the number of available threads.
 */
struct dummy_executor_t {
    dummy_executor_t() noexcept {}
    std::size_t size() const noexcept { return 1; }

    template <typename thread_aware_function_at>
    void fixed(std::size_t tasks, thread_aware_function_at&& thread_aware_function) noexcept {
        for (std::size_t task_idx = 0; task_idx != tasks; ++task_idx)
            thread_aware_function(0, task_idx);
    }

    template <typename thread_aware_function_at>
    void dynamic(std::size_t tasks, thread_aware_function_at&& thread_aware_function) noexcept {
        for (std::size_t task_idx = 0; task_idx != tasks; ++task_idx)
            if (!thread_aware_function(0, task_idx))
                break;
    }

    template <typename thread_aware_function_at>
    void parallel(thread_aware_function_at&& thread_aware_function) noexcept {
        thread_aware_function(0);
    }
};

/**
 *  @brief  An example of what a USearch-compatible key-to-key mapping should look like.
 *
 *  This is particularly helpful for "Semantic Joins", where we map entries of one collection
 *  to entries of another. In asymmetric setups, where A -> B is needed, but B -> A is not,
 *  this can be passed to minimize memory usage.
 */
struct dummy_key_to_key_mapping_t {
    struct member_ref_t {
        template <typename key_at> member_ref_t& operator=(key_at&&) noexcept { return *this; }
    };
    template <typename key_at> member_ref_t operator[](key_at&&) const noexcept { return {}; }
};

/**
 *  @brief  Checks if the provided object has a dummy type, emulating an interface,
 *          but performing no real computation.
 */
template <typename object_at> static constexpr bool is_dummy() {
    using object_t = typename std::remove_all_extents<object_at>::type;
    return std::is_same<typename std::decay<object_t>::type, dummy_predicate_t>::value || //
           std::is_same<typename std::decay<object_t>::type, dummy_callback_t>::value ||  //
           std::is_same<typename std::decay<object_t>::type, dummy_progress_t>::value ||  //
           std::is_same<typename std::decay<object_t>::type, dummy_prefetch_t>::value ||  //
           std::is_same<typename std::decay<object_t>::type, dummy_executor_t>::value ||  //
           std::is_same<typename std::decay<object_t>::type, dummy_key_to_key_mapping_t>::value;
}

template <typename, typename at> struct has_reset_gt {
    static_assert(std::integral_constant<at, false>::value, "Second template parameter needs to be of function type.");
};

template <typename check_at, typename return_at, typename... args_at>
struct has_reset_gt<check_at, return_at(args_at...)> {
  private:
    template <typename at>
    static constexpr auto check(at*) ->
        typename std::is_same<decltype(std::declval<at>().reset(std::declval<args_at>()...)), return_at>::type;
    template <typename> static constexpr std::false_type check(...);

    typedef decltype(check<check_at>(0)) type;

  public:
    static constexpr bool value = type::value;
};

/**
 *  @brief  Checks if a certain class has a member function called `reset`.
 */
template <typename at> constexpr bool has_reset() { return has_reset_gt<at, void()>::value; }

} // namespace usearch
} // namespace unum
