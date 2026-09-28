/**
 * @file helpers.cpp
 * @brief helpers 纯函数单测（无 Runtime / 无 LLM）。
 */
#include "../../tools/apex/helpers.hpp"
#include "../../tools/apex/slim.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <unordered_map>

using api::agentOk;
using api::extractAgentResult;
using api::formatCorpusText;
using api::formatPrompt;
using api::Fuse;
using api::json;
using api::l1cap;
using api::L1entry;
using api::l1Insert;
using api::parseSlim;
using api::slimToJson;
using api::steadyNowMs;
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
                                      json{{"model", "standard"},
                                           {"depth", "medium"},
                                           {"retrieval", "L2"},
                                           {"compression", 0.7},
                                           {"confidence", 0.75},
                                           {"temperature", 0.2},
                                           {"reasons", json::array({"r1"})}},
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
    assert(agentOk(
        extractAgentResult("<think>x</think><agent-result>{\"status\":\"ok\",\"payload\":\"hi\"}</agent-result>")));
    assert(!agentOk(extractAgentResult("<agent-result>{\"status\":\"delegate\"}</agent-result>")));
    assert(extractAgentResult("<agent-result>{bad json}</agent-result>").empty());
    assert(extractAgentResult("no tags here").empty());
    assert(extractAgentResult("<agent-result>{\"a\":1}").empty());
    assert(extractAgentResult("{\"a\":1}</agent-result>").empty());

    // 边界：多标签取首个；大小写不合规判失效；think 内示例不泄漏
    {
        auto multi = extractAgentResult("<agent-result>{\"status\":\"ok\",\"payload\":\"first\"}</agent-result>"
                                        "<agent-result>{\"status\":\"ok\",\"payload\":\"second\"}</agent-result>");
        assert(agentOk(multi));
        assert(multi.value("payload", "") == "first");

        assert(extractAgentResult("<AGENT-RESULT>{\"status\":\"ok\"}</AGENT-RESULT>").empty());
        assert(extractAgentResult("<Agent-Result>{\"status\":\"ok\"}</Agent-Result>").empty());

        auto nested = extractAgentResult(
            "<think>example: <agent-result>{\"status\":\"ok\",\"payload\":\"fake\"}</agent-result></think>"
            "<agent-result>{\"status\":\"ok\",\"payload\":\"real\"}</agent-result>");
        assert(agentOk(nested));
        assert(nested.value("payload", "") == "real");
    }

    // 宽松兜底：payload 内含【未转义】的双引号（3B 模型常见）→ 仍能救回
    {
        auto bad = extractAgentResult("<agent-result>{\"status\":\"ok\",\"tool_calls\":[],\"payload\":\"use \"git add "
                                      "<file>...\" to stage\"}</agent-result>");
        assert(agentOk(bad));
        assert(bad.value("payload", "").find("git add <file>") != std::string::npos);

        // 正常转义路径仍然走严格 JSON（\n 还原为换行）
        auto good = extractAgentResult(
            "<agent-result>{\"status\":\"ok\",\"tool_calls\":[],\"payload\":\"line1\\nline2\"}</agent-result>");
        assert(agentOk(good));
        assert(good.value("payload", "") == "line1\nline2");

        // delegate 也要能宽松识别
        auto del = extractAgentResult(
            "<agent-result>{\"status\":\"delegate\",\"tool_calls\":[],\"payload\":\"\"}</agent-result>");
        assert(!agentOk(del));
        assert(del.value("status", "") == "delegate");
    }

    // Fuse：触发 / 熔断 / 恢复 / 移动语义
    {
        Fuse f;
        assert(!f.tripped(5, 300));
        for (int i = 0; i < 4; ++i)
            f.fail(5, 300);
        assert(!f.tripped(5, 300));
        f.fail(5, 300); // 第 5 次打开熔断
        assert(f.tripped(5, 300));
        Fuse g = std::move(f);
        assert(g.tripped(5, 300));
        g.ok();
        assert(!g.tripped(5, 300));
        // recoverSeconds=0：一旦打开永不自动恢复（需显式 ok）
        g.fail(1, 0);
        assert(g.tripped(1, 0));
        // failThreshold=0：不计失败，永不打开
        Fuse h;
        for (int i = 0; i < 10; ++i)
            h.fail(0, 1);
        assert(!h.tripped(0, 1));
    }

    // L1：插入 / 覆盖 / TTL / LRU 淘汰
    {
        std::unordered_map<std::string, L1entry> l1;
        std::uint64_t tick = 0;
        l1Insert(l1, tick, "k", "v1", "local", "fp", 0);
        assert(l1.at("k").reply == "v1");
        assert(l1.at("k").source == "local");
        assert(l1.at("k").fingerprint == "fp");
        assert(l1.at("k").expiresAtMs == 0);
        l1Insert(l1, tick, "k", "v2", "upstream", "fp2", 0);
        assert(l1.at("k").reply == "v2");
        assert(l1.at("k").source == "upstream");

        l1Insert(l1, tick, "ttl", "v", "local", "fp", 1);
        assert(l1.at("ttl").expiresAtMs > steadyNowMs());
        assert(l1.at("ttl").expiresAtMs <= steadyNowMs() + 1000);

        // 填满 l1cap 后插入新键，驱逐 tick 最小者（最早插入的 "k"）
        std::unordered_map<std::string, L1entry> big;
        std::uint64_t bt = 0;
        for (std::size_t i = 0; i < l1cap; ++i)
            l1Insert(big, bt, "key" + std::to_string(i), "v", "local", "fp", 0);
        assert(big.size() == l1cap);
        l1Insert(big, bt, "overflow", "v", "local", "fp", 0);
        assert(big.size() == l1cap);
        assert(big.find("key0") == big.end());
        assert(big.find("overflow") != big.end());
    }

    std::puts("apex helpers: ok");
    return 0;
}