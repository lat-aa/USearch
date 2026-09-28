/**
 *  @file       agent.cpp
 *  @brief      本地 CoT agent：采样参数只经 Encoder::chat 进锁。
 */
#include "api.hpp"
#include "render.hpp"
#include "slim.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
namespace api {
namespace {static std::string envStr(char const* key) {
    char const* v = std::getenv(key);
    return v && *v ? std::string(v) : std::string();
}

/**
 * SMOKE_AGENT_FIXTURE 的确定性桩输出：仅当设置该环境变量时生效，生产路径为空。
 * ok / delegate / truncated 覆盖三路径；tools 覆盖有界工具循环（首轮发工具、次轮收敛）。
 */
static std::string fixtureChat(std::string const& mode, std::size_t round) {
    if (mode == "ok")
        return "<think>fixture</think><agent-result>{\"status\":\"ok\",\"tool_calls\":[],\"payload\":\"fixture-ok\"}</"
               "agent-result>";
    if (mode == "delegate")
        return "<think>fixture</think><agent-result>{\"status\":\"delegate\",\"tool_calls\":[],\"payload\":\"\"}</"
               "agent-result>";
    if (mode == "truncated")
        return "<think>reasoning, but the model stopped mid-stream"; // 无结束标签 → 判失效
    if (mode == "tools")
        return round == 0 ? "<agent-result>{\"status\":\"ok\",\"tool_calls\":[{\"name\":\"status\",\"args\":{}}],"
                            "\"payload\":\"\"}</agent-result>"
                          : "<think>fixture</"
                            "think><agent-result>{\"status\":\"ok\",\"tool_calls\":[],\"payload\":\"fixture-tools-ok\"}"
                            "</agent-result>";
    return "<agent-result>{\"status\":\"delegate\"}</agent-result>";
}

/**
 * 本地 agent 的 GBNF 约束：可选 <think>（须闭合），随后必须是一个 <agent-result>…</agent-result>
 * 包裹的合法 JSON。用它根治"模型只输出 <think> 不收尾"的问题（与具体模型无关）。
 */
static char const* const kAgentGrammar = R"GBNF(
root   ::= think? agent
think  ::= "<think>" [^<]* "</think>"
agent  ::= "<agent-result>" ws value "</agent-result>"
value  ::= object | array | string | number | ("true" | "false" | "null") ws
object ::= "{" ws ( string ":" ws value ("," ws string ":" ws value)* )? "}" ws
array  ::= "[" ws ( value ("," ws value)* )? "]" ws
string ::= "\"" ( [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]) )* "\"" ws
number ::= ("-"? ([0-9] | [1-9] [0-9]*)) ("." [0-9]+)? ([eE] [-+]? [0-9]+)? ws
ws     ::= [ \t\n]*
)GBNF";
} // namespace

bool agentRun(Runtime& rt, std::string const& modelName, json const& agentMsgs, std::string& payload) {
    (void)modelName;
    auto const& cfg = rt.config.agent;
    std::uint32_t ctx = rt.encoder.ctx ? rt.encoder.ctx : 4096;
    std::uint32_t budget = static_cast<std::uint32_t>(static_cast<double>(ctx) * cfg.tokenBudget);
    if (budget < 256)
        budget = 256;

    json msgs = agentMsgs; // 复制；工具结果追加进会话
    std::size_t used = 0;
    std::size_t rounds = 0;

    std::string const fixture = envStr("SMOKE_AGENT_FIXTURE");
    // 默认关闭：实测 Nanbeige 的 <think> 是特殊 token（166103），llama.cpp 语法采样器会抛
    // "empty grammar stack"。仅在模型不含此类特殊 token 时才用 APEX_AGENT_GRAMMAR=1 打开。
    bool const useGrammar = envStr("APEX_AGENT_GRAMMAR") == "1";
    // Nanbeige 4.2：模板默认强制 <think>；等价于 enable_thinking=false 的 prefill。
    // 非 Nanbeige 模型可设 APEX_AGENT_PREFILL= 空串关掉。
    std::string const prefill =
        envStr("APEX_AGENT_PREFILL").empty() ? std::string("<think>\n\n</think>\n\n") : envStr("APEX_AGENT_PREFILL");
    // 允许用文件覆盖语法（调参/试验；生产用内置 kAgentGrammar）。
    std::string grammarText = kAgentGrammar;
    if (std::string gf = envStr("APEX_AGENT_GRAMMAR_FILE"); !gf.empty()) {
        std::ifstream in(gf);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            if (!ss.str().empty())
                grammarText = ss.str();
        }
    }
    auto infer = [&](std::string& raw) {
        if (!fixture.empty()) {
            raw = fixtureChat(fixture, rounds);
            return;
        }
        raw = rt.encoder.chat(msgs, {}, -1.0f, budget, useGrammar ? &grammarText : nullptr, &prefill);
    };
    auto fail = [&]() {
        rt.agentParsefail.fetch_add(1, std::memory_order_relaxed);
        if (cfg.enableFuse)
            rt.fuse.fail(cfg.fuseFail, cfg.fuseRecover);
    };

    std::string const rawDump = envStr("APEX_AGENT_RAW");
    while (true) {
        std::string raw;
        infer(raw);
        used += estimate(raw);
        if (!rawDump.empty())
            std::fprintf(stderr, "api: agent raw round=%zu bytes=%zu\n%s\n----\n", rounds, raw.size(), raw.c_str());

        json decision = extractAgentResult(raw);
        if (decision.empty()) {
            fail();
            return false;
        }
        if (agentOk(decision)) {
            json calls = decision.value("tool_calls", json::array());
            if (!calls.is_array() || calls.empty()) {
                rt.agentOk.fetch_add(1, std::memory_order_relaxed);
                if (cfg.enableFuse)
                    rt.fuse.ok();
                payload = decision.value("payload", "");
                return true;
            }
            // ok 却带 tool_calls：协议矛盾，仍执行一轮工具后继续。
        } else if (decision.value("status", "") == "delegate") {
            return false; // 正常委托，不计失败
        }

        json calls = decision.value("tool_calls", json::array());
        if (!calls.is_array() || calls.empty()) {
            fail();
            return false;
        }
        for (auto const& tc : calls) {
            if (!tc.is_object())
                continue;
            std::string name = tc.value("name", "");
            if (name.empty())
                continue;
            json args = tc.value("args", json::object());
            json result = callTool(rt, name, args, Mcpclient{});
            std::string text;
            if (result.contains("content") && result["content"].is_array() && !result["content"].empty())
                text = result["content"][0].value("text", "");
            bool isErr = result.value("isError", false);
            msgs.push_back(textMessage("user", "tool result (" + name + (isErr ? ", error" : "") + "):\n" + text));
        }
        ++rounds;
        rt.agentRounds.fetch_add(1, std::memory_order_relaxed);
        if (rounds >= cfg.maxRounds || used >= budget) {
            // 有工具却无法在预算内收敛 → 委托；不计入熔断失败（属正常边界）。
            return false;
        }
    }
}
} // namespace api

