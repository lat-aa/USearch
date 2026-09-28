/**
 * @file decide.cpp
 * @brief decide 档位命名与降档约定（本地镜像，不拉 llama），防止 MCP/HTTP 展示名漂移。
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>

enum class Model : std::uint8_t { Weak, Standard, Strong };
enum class Depth : std::uint8_t { Shallow, Medium, Deep };
enum class Retrieval : std::uint8_t { L0, L1, L2, L3 };

static char const* modelName(Model m) noexcept {
    switch (m) {
    case Model::Weak:
        return "weak";
    case Model::Standard:
        return "standard";
    case Model::Strong:
        return "strong";
    }
    return "standard";
}

static Model modelDowngrade(Model m) noexcept {
    switch (m) {
    case Model::Weak:
        return Model::Weak;
    case Model::Standard:
        return Model::Weak;
    case Model::Strong:
        return Model::Standard;
    }
    return Model::Weak;
}

static char const* depthName(Depth d) noexcept {
    switch (d) {
    case Depth::Shallow:
        return "shallow";
    case Depth::Medium:
        return "medium";
    case Depth::Deep:
        return "deep";
    }
    return "medium";
}

static Depth depthDowngrade(Depth d) noexcept {
    switch (d) {
    case Depth::Shallow:
        return Depth::Shallow;
    case Depth::Medium:
        return Depth::Shallow;
    case Depth::Deep:
        return Depth::Medium;
    }
    return Depth::Shallow;
}

// Release（NDEBUG）下 assert 会被剥掉 → 函数看似未用；改用显式 CHECK，测试在任意构建档都真跑。
static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                       \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

int main() {
    CHECK(std::strcmp(modelName(Model::Weak), "weak") == 0);
    CHECK(std::strcmp(modelName(Model::Strong), "strong") == 0);
    CHECK(modelDowngrade(Model::Strong) == Model::Standard);
    CHECK(modelDowngrade(Model::Standard) == Model::Weak);
    CHECK(modelDowngrade(Model::Weak) == Model::Weak);

    CHECK(std::strcmp(depthName(Depth::Deep), "deep") == 0);
    CHECK(depthDowngrade(Depth::Deep) == Depth::Medium);
    CHECK(depthDowngrade(Depth::Shallow) == Depth::Shallow);

    CHECK(static_cast<int>(Retrieval::L0) == 0);
    CHECK(static_cast<int>(Retrieval::L3) == 3);

    if (failures) {
        std::fprintf(stderr, "apex decide: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("apex decide: ok");
    return 0;
}
