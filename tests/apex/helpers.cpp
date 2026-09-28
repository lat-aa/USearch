/**
 * @file helpers.cpp
 * @brief gate helpers 纯函数单测（无 Runtime / 无 LLM）。
 */
#include "../../tools/apex/helpers.hpp"
#include "../../tools/apex/slim.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

using api::Hitdoc;
using api::Packcfg;
using api::Rulehit;
using api::buildPack;
using api::detectConflicts;
using api::evidenceOf;
using api::finalizeStatus;
using api::formatCorpus;
using api::formatPrompt;
using api::kindIs;
using api::metaConflicted;
using api::metaStr;
using api::parseGateModel;
using api::parseSlim;
using api::slimToJson;
using api::json;

int main() {
    json meta = {{"kind", "cache"}, {"fingerprint", "abc"}};
    assert(kindIs(meta, "cache"));
    assert(!kindIs(meta, "memory"));
    assert(kindIs(json::object(), "memory")); // 无 kind → memory 缺省
    assert(metaStr(meta, "fingerprint") == "abc");
    assert(metaStr(meta, "missing").empty());
    assert(!metaConflicted(meta));
    assert(metaConflicted(json {{"conflict", true}}));
    assert(!metaConflicted(json {{"conflict", "yes"}})); // 非 bool 不算

    assert(parseGateModel("noise {\"status\":\"answered\",\"self\":0.9} tail")["status"] == "answered");
    assert(parseGateModel("not json").empty());

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

    std::string hardName = "names";
    std::vector<std::string const*> hard {&hardName};
    std::vector<std::string const*> ban {&hardName};
    Hitdoc bad;
    bad.id = "m1";
    bad.text = "use foo_bar please";
    bad.meta = json::object();
    auto conflicts = detectConflicts(hard, ban, "clean task", {{bad, 0.1f}});
    assert(conflicts.is_array() && !conflicts.empty());

    auto clean = detectConflicts(hard, ban, "clean task", {});
    assert(clean.is_array() && clean.empty());

    // task 本身含违规标识
    auto fromTask = detectConflicts(hard, ban, "x foo_bar y", {});
    assert(!fromTask.empty());

    std::vector<Rulehit> rules {{"default", "body", 0, 0.f}, {"sem", "s", 2, 0.8f}};
    float ev = evidenceOf({{bad, 0.2f}}, rules);
    assert(ev > 0.f && ev <= 1.f);

    api::Gateweights w;
    w.threshold = 0.85f;
    w.minlen = 8;
    json model = {{"status", "answered"}, {"self", 0.95}, {"reply", "long enough answer text"},
                  {"conflicts", json::array()}, {"missing", json::array()}};
    auto fin = finalizeStatus(model, 0.9f, w);
    assert(fin["status"] == "answered");

    model["conflicts"] = json::array({{{"rule", "names"}, {"why", "x"}}});
    fin = finalizeStatus(model, 0.9f, w);
    assert(fin["status"] == "pack" || fin["status"] == "refuse");

    Packcfg cfg;
    cfg.packtok = 4096;
    cfg.baseSecs = 2;
    Hitdoc mem {"id1", "memory text\n\nmore", json {{"kind", "memory"}}};
    Rulehit always {"default", "# title\n\npara", 0, 0.f};
    auto pack = buildPack(cfg, "task", {{mem, 0.5f}}, {always},
                          json {{"retrieval", "L2"}, {"compression", 0.7}}, json::array());
    assert(pack.contains("items") && pack["items"].is_array() && !pack["items"].empty());

    std::string prompt = formatPrompt(json::array({{{"name", "default"}, {"body", "hello"}}}),
                                      json {{"model", "standard"},
                                            {"depth", "medium"},
                                            {"retrieval", "L2"},
                                            {"compression", 0.7},
                                            {"confidence", 0.75},
                                            {"temperature", 0.2},
                                            {"reasons", json::array({"r1"})}},
                                      json::array(), nullptr);
    assert(prompt.find("Local knowledge JSON follows.") == 0);
    std::string corpus = formatCorpus(prompt);
    assert(corpus.find("## prompt\n") == 0);
    assert(corpus.find("Local knowledge JSON follows.") == std::string::npos);
    assert(corpus.find("路由") != std::string::npos);
    assert(corpus.find("### default") != std::string::npos);

    std::puts("apex helpers: ok");
    return 0;
}
