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

int main() {
    assert(std::strcmp(modelName(Model::Weak), "weak") == 0);
    assert(std::strcmp(modelName(Model::Strong), "strong") == 0);
    assert(modelDowngrade(Model::Strong) == Model::Standard);
    assert(modelDowngrade(Model::Standard) == Model::Weak);
    assert(modelDowngrade(Model::Weak) == Model::Weak);

    assert(std::strcmp(depthName(Depth::Deep), "deep") == 0);
    assert(depthDowngrade(Depth::Deep) == Depth::Medium);
    assert(depthDowngrade(Depth::Shallow) == Depth::Shallow);

    assert(static_cast<int>(Retrieval::L0) == 0);
    assert(static_cast<int>(Retrieval::L3) == 3);

    std::puts("apex decide: ok");
    return 0;
}
