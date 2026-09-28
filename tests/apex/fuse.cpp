/**
 * @file fuse.cpp
 * @brief Fuse 熔断 trip / recover 合同测（纯逻辑，无 llama）。
 */
#include "../../tools/apex/render.hpp"

#include <cassert>
#include <cstdio>

int main() {
    using api::Fuse;
    Fuse f;
    assert(!f.tripped(5, 300));
    for (int i = 0; i < 5; ++i)
        f.fail(5, 300);
    assert(f.tripped(5, 300));
    f.ok();
    assert(!f.tripped(5, 300));
    // recoverSeconds=0：一旦打开永不自动恢复（需显式 ok）
    f.fail(1, 0);
    assert(f.tripped(1, 0));
    std::puts("apex fuse: ok");
    return 0;
}
