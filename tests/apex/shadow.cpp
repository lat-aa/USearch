/**
 * @file shadow.cpp
 * @brief shadow 量化往返：与 sq8::quantizeRow 同构的纯函数合同（不链 llama）。
 */
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static float quantizeRow(float const* row, std::int8_t* dst, std::size_t dim) {
    float maxAbs = 0;
    for (std::size_t i = 0; i < dim; ++i)
        maxAbs = maxAbs > std::fabs(row[i]) ? maxAbs : std::fabs(row[i]);
    float scale = maxAbs > 0 ? maxAbs / 127.0f : 1.0f;
    for (std::size_t i = 0; i < dim; ++i) {
        int v = static_cast<int>(std::lround(row[i] / scale));
        if (v > 127)
            v = 127;
        if (v < -127)
            v = -127;
        dst[i] = static_cast<std::int8_t>(v);
    }
    return scale;
}

int main() {
    float row[4] = {0.5f, -0.25f, 0.0f, 0.1f};
    std::int8_t q[4];
    float s = quantizeRow(row, q, 4);
    assert(s > 0);
    float recon = static_cast<float>(q[0]) * s;
    assert(std::fabs(recon - 0.5f) < 0.02f);
    std::puts("apex shadow: ok");
    return 0;
}
