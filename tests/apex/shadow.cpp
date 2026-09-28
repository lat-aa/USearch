/**
 * @file shadow.cpp
 * @brief SQ8 量化往返：验证生产 sq8::quantizeRow / coarseScore / epsFromMaxScale。
 */
#include "../../tools/apex/shadow.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    using namespace api::sq8;

    // 往返：|x - q*s/127| ≤ 一格（s/127）
    float row[4] = {0.5f, -0.25f, 0.0f, 0.1f};
    std::int8_t q[4];
    float s = quantizeRow(row, q, 4);
    assert(s > 0.0f);
    for (int i = 0; i < 4; ++i) {
        float recon = static_cast<float>(q[i]) * s / 127.0f;
        assert(std::fabs(recon - row[i]) <= s / 127.0f + 1e-6f);
    }

    // 全零行：scale=0，量化全 0
    float zero[3] = {0, 0, 0};
    std::int8_t qz[3];
    assert(quantizeRow(zero, qz, 3) == 0.0f);
    assert(qz[0] == 0 && qz[1] == 0 && qz[2] == 0);

    // 非有限输入 → -1 哨兵，整行清零
    float nanrow[3] = {0.1f, std::nanf(""), 0.2f};
    std::int8_t qn[3] = {9, 9, 9};
    assert(quantizeRow(nanrow, qn, 3) == -1.0f);
    assert(qn[0] == 0 && qn[1] == 0 && qn[2] == 0);

    // 哨兵行永不被选：coarseScore = -inf
    assert(!std::isfinite(coarseScore(123, -1.0f)));
    assert(coarseScore(0, 0.0f) == 0.0f);

    // 候选预算下界 + eps 单调性
    assert(candidateBudget(1) == 1024);
    assert(candidateBudget(1000) == 16000);
    assert(epsFromMaxScale(64, 1.0f) > epsFromMaxScale(16, 1.0f));
    assert(epsFromMaxScale(16, 2.0f) > epsFromMaxScale(16, 1.0f));

    std::puts("apex shadow: ok");
    return 0;
}
