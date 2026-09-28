/**
 *  @file       api.cpp
 *  @brief      进程组装：Runtime::open 与 main；平面实现见 encode/store/openai/worker。
 */

#include "api.hpp"
#include "render.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>

namespace api {

namespace {
char const* kMissingToml = "missing config; copy .config/config.example.toml to .config/config.toml";
}

expected_gt<Runtime> Runtime::open(fs::path const& root) {
    expected_gt<Runtime> out;
    fs::path cfgPath = root / ".config" / "config.toml";
    if (!fs::is_regular_file(cfgPath))
        return out.failed(kMissingToml);
    auto cfg = Config::load(cfgPath);
    if (!cfg)
        return out.failed(cfg.error.release());
    Runtime rt;
    rt.root = root;
    rt.config = std::move(cfg.result);

    auto lex = loadLexicon(joinRoot(root, rt.config.decide.lexicon));
    if (!lex)
        return out.failed(lex.error.release());
    auto decider = Decider::open(rt.config.decide, std::move(lex.result));
    if (!decider)
        return out.failed(decider.error.release());
    rt.decider = std::move(decider.result);

    fs::path gguf = joinRoot(root, rt.config.gguf);
    if (error_t err = rt.encoder.open(gguf, rt.config.ctx, rt.config.gpu, rt.config.threads, rt.config.chat.temperature,
                                      rt.config.chat.max);
        err)
        return out.failed(err.release());
    // 专用嵌入须在建 Store 前成功或明确回退，否则维度与 ANN 不一致。
    if (!rt.config.embed.gguf.empty()) {
        fs::path eg = joinRoot(root, rt.config.embed.gguf);
        int egpu = rt.config.embed.gpu == -1000 ? rt.config.gpu : rt.config.embed.gpu;
        std::uint32_t ectx = rt.config.ctx ? rt.config.ctx : 1024;
        if (error_t e = rt.encoder.openEmbed(eg, ectx, egpu, rt.config.pooling); e) {
            char const* msg = e.release();
            std::fprintf(stderr, "api: 嵌入模型 GPU 加载失败(%s)\n", msg ? msg : "?");
            if (egpu != 0) {
                std::fprintf(stderr, "api: 回退嵌入模型到 CPU (gpu=0)\n");
                if (error_t e2 = rt.encoder.openEmbed(eg, ectx, 0, rt.config.pooling); e2) {
                    char const* m2 = e2.release();
                    std::fprintf(stderr, "api: 嵌入模型 CPU 加载也失败(%s)，复用 chat 模型\n", m2 ? m2 : "?");
                }
            }
        }
    }
    auto store = Store::make(rt.encoder.dimensions, joinRoot(root, rt.config.index), joinRoot(root, rt.config.base),
                             rt.config.shadow);
    if (!store)
        return out.failed(store.error.release());
    rt.store = std::move(store.result);
    try {
        rt.rules = loadRules(joinRoot(root, rt.config.rules));
    } catch (...) {
        return out.failed("rules load failed");
    }
    rt.policyFp = policyFingerprint(rt.rules);
    warmRuleVecs(rt);
    out.result = std::move(rt);
    return out;
}

fs::path Runtime::workspace() const { return joinRoot(root, config.workspace); }

std::string Runtime::shell(std::string const& command) const {
    fs::path ws = workspace();
#if defined(_WIN32)
    std::string full = "cd /d \"" + ws.string() + "\" && " + command;
#else
    std::string full = "cd \"" + ws.string() + "\" && " + command;
#endif
    FILE* pipe =
#if defined(_WIN32)
        _popen(full.c_str(), "r");
#else
        popen(full.c_str(), "r");
#endif
    if (!pipe)
        return "failed to spawn shell";
    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe))
        output += buffer;
#if defined(_WIN32)
    int code = _pclose(pipe);
#else
    int code = pclose(pipe);
#endif
    output += "\nexit=" + std::to_string(code);
    return output;
}

expected_gt<json> Runtime::saveExperience(Experience const& exp) {
    expected_gt<json> out;
    auto path = saveMark(joinRoot(root, config.knowledge), exp);
    if (!path)
        return out.failed(path.error.release());
    std::string id = "exp-" + std::to_string(fnv1a64(path.result));
    Doc doc;
    doc.id = id;
    doc.text = exp.title + "\n" + exp.summary;
    doc.meta = {{"path", path.result}, {"kind", "memory"}};
    auto vector = encoder.embed(doc.text);
    if (error_t err = store.upsert(std::move(doc), vector); err)
        return out.failed(err.release());
    out.result = {{"path", path.result}, {"id", id}};
    return out;
}

int runAgent(Runtime& rt, std::string const& instruction) {
    auto vector = rt.encoder.embed(instruction);
    auto hits = rt.store.search(vector, 8);
    auto matched = resolveRules(rt, Rulequery{instruction, {}, {}});
    json pack = {{"hits", json::array()}, {"rules", json::array()}};
    for (auto const& [doc, score] : hits)
        pack["hits"].push_back({{"id", doc.id}, {"text", doc.text}, {"score", score}});
    for (auto const& r : matched)
        pack["rules"].push_back(
            {{"name", r.rule.name}, {"activation", activationName(r.activation)}, {"body", r.rule.body}});
    std::string system = "Local knowledge JSON follows. Decide enough vs generate.\n" + pack.dump();
    json runMessages = json::array({textMessage("system", std::move(system)), textMessage("user", instruction)});
    std::string reply = rt.encoder.chat(runMessages);
    std::cout << reply << '\n';
    Experience exp;
    exp.title = "run";
    exp.summary = instruction.substr(0, (std::min)(instruction.size(), std::size_t{200}));
    exp.tags = {"run"};
    exp.outcome = "ok";
    auto saved = rt.saveExperience(exp);
    if (!saved) {
        std::fprintf(stderr, "save failed: %s\n", saved.error.release());
        return 1;
    }
    std::cout << saved.result.dump(2) << '\n';
    return 0;
}

} // namespace api

int main(int argc, char** argv) {
    std::string command = argc > 1 ? argv[1] : "serve";
    auto root = api::findRoot();
    auto runtime = api::Runtime::open(root);
    if (!runtime) {
        std::fprintf(stderr, "api: %s\n", runtime.error.release());
        return 1;
    }
    if (command == "serve")
        return api::serve(runtime.result);
    if (command == "run") {
        std::string instruction;
        for (int i = 2; i < argc; ++i) {
            if (i > 2)
                instruction.push_back(' ');
            instruction += argv[i];
        }
        if (instruction.empty()) {
            std::fprintf(stderr, "用法: api serve | api run \"<指令>\"\n");
            return 2;
        }
        return api::runAgent(runtime.result, instruction);
    }
    std::fprintf(stderr, "用法: api serve | api run \"<指令>\"\n");
    return 2;
}
