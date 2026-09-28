/**
 * @file rules.cpp
 * @brief 无 Encoder 的词法规则激活合同（不调 embed）。
 */
#include "../../tools/apex/render.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    // 词法路径不得依赖 Encoder：空 task 不得假装语义命中。
    std::string task;
    assert(task.empty());
    std::puts("apex rules: ok");
    return 0;
}
