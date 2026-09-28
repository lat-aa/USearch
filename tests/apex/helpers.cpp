/**
 * @file helpers.cpp
 * @brief helpers 纯函数单测（无 Runtime / 无 LLM）。
 */
#include "../../tools/apex/helpers.hpp"
#include "../../tools/apex/slim.hpp"

#include <cassert>
#include <cstdio>

using api::agentOk;
using api::extractAgentResult;
using api::formatCorpusText;
using api::formatPrompt;
using api::json;
using api::parseSlim;
using api::slimToJson;
using api::stripThink;

int main() {
    // parseSlim：固定 schema，未知键跳过，坏 JSON 失败
    {
        auto ok = parseSlim(R"({"task":"hello","files":["a.cpp"],"latency":12,"extra":{"x":1}})");
        assert(ok.ok);
        assert(ok.task == "hello");
        assert(ok.files.size() == 1 && ok.files[0] == "a.cpp");
        assert(ok.latency && *ok.latency == 12);
        auto j = slimToJson(ok);
        assert(j["task"] == "hello");
        assert(!parseSlim("not-json").ok);
        assert(!parseSlim(R"({"task":)").ok);
    }

    std::string prompt = formatPrompt(json::array({{{"name", "default"}, {"body", "hello"}}}),
                                      json {{"model", "standard"}, {"depth", "medium"}, {"retrieval", "L2"},
                                            {"compression", 0.7}, {"confidence", 0.75},
                                            {"temperature", 0.2}, {"reasons", json::array({"r1"})}},
                                      json::array(), nullptr);
    assert(prompt.find("Local knowledge JSON follows.") == 0);
    std::string corpus = formatCorpusText(prompt);
    assert(corpus.find("## prompt\n") == 0);
    assert(corpus.find("Local knowledge JSON follows.") == std::string::npos);
    assert(corpus.find("路由") != std::string::npos);
    assert(corpus.find("### default") != std::string::npos);

    // stripThink
    assert(stripThink("<think>a\nb</think>hello") == "hello");
    assert(stripThink("<|im_start|>note").find("<|im_start|>") == std::string::npos);
    assert(stripThink("<think>only</think>").empty());
    assert(stripThink("  plain  ") == "plain");

    // extractAgentResult / agentOk：标签隔离 + 解析兜底
    assert(agentOk(extractAgentResult("<think>x</think><agent-result>{\"status\":\"ok\",\"payload\":\"hi\"}</agent-result>")));
    assert(!agentOk(extractAgentResult("<agent-result>{\"status\":\"delegate\"}</agent-result>")));
    assert(extractAgentResult("<agent-result>{bad json}</agent-result>").empty());
    assert(extractAgentResult("no tags here").empty());
    assert(extractAgentResult("<agent-result>{\"a\":1}").empty());
    assert(extractAgentResult("{\"a\":1}</agent-result>").empty());

    std::puts("apex helpers: ok");
    return 0;
}