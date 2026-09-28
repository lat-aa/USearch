/**
 * @file worker.cpp
 * @brief Worker 硬让路合同：验证生产 api::workerHot 的 busy / 窗口边界。
 */
#include "../../tools/apex/worker.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
    using api::workerHot;
    assert(workerHot(true, 0, 10000));            // chat 忙 → 热
    assert(workerHot(false, 9000, 10000));        // 窗口内 → 热
    assert(!workerHot(false, 7000, 10000));       // 窗口外 → 冷
    assert(!workerHot(false, 0, 10000));          // 无用户活动 → 冷
    assert(workerHot(true, 0, 10000, 5000));      // busy 优先于窗口
    assert(!workerHot(false, 8000, 10000, 1500)); // 自定义窗口外
    std::puts("apex worker: ok");
    return 0;
}
