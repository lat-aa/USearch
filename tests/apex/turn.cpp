/**
 * @file turn.cpp
 * @brief Turnstats 人读 corpus / prompt 契约单测（只链 helpers，无 llama）。
 */
#include "../../tools/apex/helpers.hpp"

#include <cassert>
#include <cstdio>

using api::formatCorpus;
using api::formatPrompt;
using api::json;

int main() {
    json rules = json::array();
    rules.push_back({{"name", "names"}, {"body", "# 命名\r\n\r\n禁止 `_`\r\n"}});
    json decision = {{"model", "standard"},
                     {"depth", "medium"},
                     {"retrieval", "L2"},
                     {"compression", 0.7},
                     {"confidence", 0.8},
                     {"temperature", 0.1},
                     {"reasons", json::array({"complexity 49 in [40, 70) (mid)"})}};
    std::string prompt = formatPrompt(rules, decision, json::array(), nullptr);
    assert(prompt.find("\"body\": \"...\"") == std::string::npos);
    assert(prompt.find("names") != std::string::npos);

    // 无真实 pack：formatPrompt 不得写入 pack 键
    assert(prompt.find("\"pack\"") == std::string::npos);

    json pack = {{"items", json::array({{{"id", "x"}, {"span", "y"}}})}};
    std::string withPack = formatPrompt(rules, decision, json::array(), &pack);
    assert(withPack.find("\"pack\"") != std::string::npos);

    std::string corpus = formatCorpus(prompt);
    assert(corpus.rfind("## prompt\n", 0) == 0);
    assert(corpus.find("路由 standard") != std::string::npos);
    assert(corpus.find("### names") != std::string::npos);
    assert(corpus.find("\\r\\n") == std::string::npos); // 人读已去 CR 转义刷屏

    // 非标准 prompt 回退原文
    std::string odd = formatCorpus("plain text only");
    assert(odd.find("plain text only") != std::string::npos);

    std::puts("apex turn: ok");
    return 0;
}
