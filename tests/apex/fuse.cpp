/**
 * @file fuse.cpp
 * @brief Fuse 熔断 trip / recover 合同测（无 llama）。
 */
#include "../../tools/apex/fuse.hpp"

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
    std::puts("apex fuse: ok");
    return 0;
}
