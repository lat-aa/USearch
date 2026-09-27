/**
 * @file verdict.hpp
 * @brief gate 终态纯函数：红线扫描、相似度、混合置信、answered|pack|refuse。
 *
 * 无 Runtime / 无 LLM，供 gate.cpp 与 tests/apex 共用（避免测试链拉 httplib/llama）。
 * 命名遵守 .config/rules/names.md：单单词文件名，禁止 _/-。
 */
#pragma once

#include <algorithm>
#include <string_view>

namespace api {

/** 夹紧到 [0, 1]。 */
inline float clamp01(float x) noexcept {
    return (std::max)(0.0f, (std::min)(1.0f, x));
}

inline bool isIdentChar(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/**
 * 红线：正文是否出现违反 names.md 的标识形态（字母数字夹着 `_` 或 `-`）。
 * 不枚举旧名；表名是 queue、工具是 observe/save，禁止再写蛇形/中划线别名。
 */
inline bool scanRed(std::string_view text) noexcept {
    char const* data = text.data();
    std::size_t const n = text.size();
    if (n < 3)
        return false;
    for (std::size_t i = 1; i + 1 < n; ++i) {
        unsigned char const cur = static_cast<unsigned char>(data[i]);
        if (cur != '_' && cur != '-')
            continue;
        unsigned char const prev = static_cast<unsigned char>(data[i - 1]);
        unsigned char const next = static_cast<unsigned char>(data[i + 1]);
        if (isIdentChar(prev) && isIdentChar(next))
            return true;
    }
    return false;
}

/** cosine 距离（或偶发相似度）→ [0,1] 相似度。 */
inline float asSim(float distanceOrSim) noexcept {
    if (distanceOrSim < 0.0f)
        return 0.0f;
    if (distanceOrSim <= 1.0f)
        return clamp01(1.0f - distanceOrSim);
    return 0.0f;
}

/**
 * 混合置信：answerConfidence = wevid*evidence + wself*self。
 * conflicts 非空时调用方必须禁止 answered（fail-closed）。
 */
inline float mixConfidence(float evidence, float self, float wevid, float wself) noexcept {
    return clamp01(wevid * clamp01(evidence) + wself * clamp01(self));
}

/**
 * 门控终态：冲突/缺信息/置信不足时不得 answered。
 * @return "answered" | "pack" | "refuse"
 */
inline char const* verdictOf(bool hasConflicts, bool hasMissing, float conf, float threshold,
                             char const* modelStatus, std::size_t replyLen,
                             std::size_t minlen) noexcept {
    if (hasConflicts) {
        if (modelStatus && std::string_view(modelStatus) == "refuse")
            return "refuse";
        return "pack";
    }
    if (hasMissing)
        return "pack";
    if (conf < threshold)
        return "pack";
    if (modelStatus && std::string_view(modelStatus) == "answered" && replyLen < minlen)
        return "pack";
    if (modelStatus && (std::string_view(modelStatus) == "answered" || std::string_view(modelStatus) == "refuse"))
        return modelStatus;
    return "pack";
}

} // namespace api
