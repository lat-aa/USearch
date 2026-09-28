/**
 *  @file       decide.hpp
 *  @brief      Policy：确定性级联决策器（词表 + 阈值，无 I/O）。
 */
#pragma once

#include "types.hpp"

namespace api {

char const* modelName(Model m) noexcept;
char const* depthName(Depth d) noexcept;
char const* retrievalName(Retrieval r) noexcept;
Model modelDowngrade(Model m) noexcept;
Depth depthDowngrade(Depth d) noexcept;
Retrieval retrievalDowngrade(Retrieval r) noexcept;

struct Decider {
    std::uint64_t speedMs = 0;
    std::uint8_t highComplexity = 0;
    std::uint8_t lowComplexity = 0;
    std::uint8_t margin = 0;
    Temppolicy temperature{};
    std::uint8_t wmedium = 0;
    std::uint8_t wcomplex = 0;
    std::size_t maxtask = 0;
    std::size_t maxfile = 0;
    float keeplow = 0;
    float keepmid = 0;
    float keephigh = 0;
    std::array<std::size_t, 4> topk{};
    std::vector<std::string> complex;
    std::vector<std::string> medium;

    /** 从配置 + 已载入词表构造；词表须非空。 */
    static expected_gt<Decider> open(Decideconfig const& cfg, Lexicon lexicon);
    Features features(Decideinput const& input) const;
    Decision decide(Decideinput const& input) const;
    Decision decideFrom(Features const& f) const;
    /** Retrieval → config.decide.topk[L0..L3]。 */
    std::size_t topkFor(Retrieval r) const noexcept;
    /** compression 保留比 → 规则裁剪段数：≥keepmid→3，≥keeplow→2，否则 1。 */
    int sectionsFor(float compression) const noexcept;
};

expected_gt<Lexicon> loadLexicon(fs::path const& path);

/** 解析 route/decide 请求体；键名：task|query、files、latency、hints。 */
Decideinput decideinputFromJson(json const& body);
/** 解析单个 hint 字符串；未知值勿调用（由 decideinputFromJson 白名单过滤）。 */
Hint parseHint(std::string const& s);

} // namespace api
