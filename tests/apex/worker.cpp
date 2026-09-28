/**
 * @file worker.cpp
 * @brief Worker 硬让路合同：chatBusy 或 lastUserMs 窗口内视为用户热路径。
 */
#include <cassert>
#include <cstdint>
#include <cstdio>

static bool userHot(int chatBusy, std::int64_t lastMs, std::int64_t nowMs) {
    if (chatBusy != 0)
        return true;
    return lastMs != 0 && nowMs - lastMs < 2000;
}

int main() {
    assert(userHot(1, 0, 10000));
    assert(userHot(0, 9000, 10000));
    assert(!userHot(0, 7000, 10000));
    assert(!userHot(0, 0, 10000));
    std::puts("apex worker: ok");
    return 0;
}
