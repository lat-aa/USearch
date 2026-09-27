/**
 * @file verdict.cpp
 * @brief gate 终态纯函数单测（scanRed / 混合置信 / fail-closed），无网络无 LLM。
 */
#include "../../tools/apex/verdict.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

using api::asSim;
using api::mixConfidence;
using api::scanRed;
using api::verdictOf;

static void expectNear(float a, float b, float eps = 1e-5f) {
    assert(std::fabs(a - b) <= eps);
}

int main() {
    assert(!scanRed("hello world"));
    assert(!scanRed("observe save queue modelReady"));
    // 探针故意含 _ / -，验证红线；不写进产品标识表。
    assert(scanRed("x foo_bar y"));
    assert(scanRed("foo-bar"));
    assert(!scanRed("a_"));
    assert(!scanRed("_b"));
    assert(!scanRed("-"));

    expectNear(asSim(0.0f), 1.0f);
    expectNear(asSim(1.0f), 0.0f);
    expectNear(asSim(0.2f), 0.8f);
    expectNear(asSim(-1.0f), 0.0f);
    expectNear(asSim(2.0f), 0.0f);

    expectNear(mixConfidence(0.8f, 0.9f, 0.45f, 0.55f), 0.855f);

    assert(std::strcmp(verdictOf(true, false, 0.99f, 0.85f, "answered", 100, 8), "pack") == 0);
    assert(std::strcmp(verdictOf(true, false, 0.99f, 0.85f, "refuse", 100, 8), "refuse") == 0);
    assert(std::strcmp(verdictOf(false, false, 0.5f, 0.85f, "answered", 100, 8), "pack") == 0);
    assert(std::strcmp(verdictOf(false, false, 0.9f, 0.85f, "answered", 100, 8), "answered") == 0);
    assert(std::strcmp(verdictOf(false, false, 0.9f, 0.85f, "answered", 3, 8), "pack") == 0);

    std::puts("apex verdict: ok");
    return 0;
}
