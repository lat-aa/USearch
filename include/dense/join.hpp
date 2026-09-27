/**
 *  @file       join.hpp
 *  @brief      index_dense_t / big 别名与自由函数 join。
 */
#pragma once
#include <dense/index.hpp>
namespace unum {
namespace usearch {

using index_dense_t = index_dense_gt<>;
using index_dense_big_t = index_dense_gt<uuid_t, uint40_t>;

/**
 *  @brief  Adapts the Male-Optimal Stable Marriage algorithm for unequal sets
 *          to perform fast one-to-one matching between two large collections
 *          of vectors, using approximate nearest neighbors search.
 *
 *  @param[inout] man_to_woman Container to map ::first keys to ::second.
 *  @param[inout] woman_to_man Container to map ::second keys to ::first.
 *  @param[in] executor Thread-pool to execute the job in parallel.
 *  @param[in] progress Callback to report the execution progress.
 */
template < //

    typename men_key_at,    //
    typename women_key_at,  //
    typename men_slot_at,   //
    typename women_slot_at, //

    typename man_to_woman_at = dummy_key_to_key_mapping_t, //
    typename woman_to_man_at = dummy_key_to_key_mapping_t, //
    typename executor_at = dummy_executor_t,               //
    typename progress_at = dummy_progress_t                //
    >
static join_result_t join(                                    //
    index_dense_gt<men_key_at, men_slot_at> const& men,       //
    index_dense_gt<women_key_at, women_slot_at> const& women, //

    index_join_config_t config = {},                    //
    man_to_woman_at&& man_to_woman = man_to_woman_at{}, //
    woman_to_man_at&& woman_to_man = woman_to_man_at{}, //
    executor_at&& executor = executor_at{},             //
    progress_at&& progress = progress_at{}) {

    return men.join(                                 //
        women, config,                               //
        std::forward<woman_to_man_at>(woman_to_man), //
        std::forward<man_to_woman_at>(man_to_woman), //
        std::forward<executor_at>(executor),         //
        std::forward<progress_at>(progress));
}

} // namespace usearch
} // namespace unum
