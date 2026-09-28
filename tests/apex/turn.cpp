/**
 * @file turn.cpp
 * @brief Turnstats 人读 corpus / prompt / 规范消息契约单测（只链 helpers，无 llama）。
 */
#include "../../tools/apex/helpers.hpp"

#include <cassert>
#include <cstdio>

using api::formatCorpus;
using api::formatCorpusText;
using api::formatPrompt;
using api::json;
using api::messagesFromResponses;
using api::normalizeContent;
using api::normalizeMessages;
using api::textMessage;

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

    // 规范消息：{role, content:[{type:"text",text}]}
    json messages = json::array({textMessage("system", prompt)});
    assert(messages[0]["role"] == "system");
    assert(messages[0]["content"].is_array() && messages[0]["content"].size() == 1);
    assert(messages[0]["content"][0]["type"] == "text");
    assert(messages[0]["content"][0]["text"].get<std::string>().find("Local knowledge JSON follows.") == 0);

    std::string corpus = formatCorpus(messages);
    assert(corpus.rfind("## prompt\n", 0) == 0);
    assert(corpus.find("路由 standard") != std::string::npos);
    assert(corpus.find("### names") != std::string::npos);
    assert(corpus.find("\\r\\n") == std::string::npos); // 人读已去 CR 转义刷屏

    // 非标准 prompt 回退原文
    std::string odd = formatCorpusText("plain text only");
    assert(odd.find("plain text only") != std::string::npos);

    // content 归一：只留文本部件，丢非文本
    {
        json parts = json::array();
        json p1;
        p1["type"] = "input_text";
        p1["text"] = "hi";
        json p2;
        p2["type"] = "input_image";
        p2["image_url"] = "x";
        parts.push_back(p1);
        parts.push_back(p2);
        parts.push_back("raw");
        json nc = normalizeContent(parts);
        assert(nc.is_array() && nc.size() == 2);
        assert(nc[0]["type"] == "text" && nc[0]["text"] == "hi");
        assert(nc[1]["text"] == "raw");
    }

    // role 归一：developer→system，空→user
    {
        json msgs = json::array();
        json a;
        a["role"] = "developer";
        a["content"] = "a";
        json b;
        b["role"] = "";
        b["content"] = "b";
        msgs.push_back(a);
        msgs.push_back(b);
        json nm = normalizeMessages(msgs);
        assert(nm.size() == 2);
        assert(nm[0]["role"] == "system" && nm[0]["content"][0]["type"] == "text");
        assert(nm[1]["role"] == "user" && nm[1]["content"][0]["text"] == "b");
    }

    // Responses 入参：instructions + 多轮 input；function_call_output 跳过
    {
        json body;
        body["instructions"] = "SYS";
        json input = json::array();
        json m1;
        m1["type"] = "message";
        m1["role"] = "user";
        json c1 = json::array();
        json t1;
        t1["type"] = "input_text";
        t1["text"] = "U1";
        c1.push_back(t1);
        m1["content"] = c1;
        json fc;
        fc["type"] = "function_call_output";
        fc["output"] = "x";
        input.push_back(m1);
        input.push_back(fc);
        input.push_back("U2");
        body["input"] = input;
        json msgs = messagesFromResponses(body);
        assert(msgs.size() == 3);
        assert(msgs[0]["role"] == "system" && msgs[0]["content"][0]["text"] == "SYS");
        assert(msgs[1]["role"] == "user" && msgs[1]["content"][0]["text"] == "U1");
        assert(msgs[2]["role"] == "user" && msgs[2]["content"][0]["text"] == "U2");
    }

    std::puts("apex turn: ok");
    return 0;
}