/**
 *  @file       api.cpp
 *  @author     USearch contributors
 *  @brief      本地 HTTP / MCP 进程入口：装配 Runtime，OpenAI /v1，分发 serve / run。
 *  @date       2026-09-27
 *
 *  # 构建
 *  @code
 *  cmake -B build -DUSEARCH_BUILD_API=ON
 *  cmake --build build --config Release --target api
 *  @endcode
 */

#include "api.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace api {

void mountOpenai(httplib::Server& svr, Runtime& rt,
                 std::function<bool(httplib::Request const&, httplib::Response&)> gate) {
    svr.Get("/v1/models", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        setJson(res, {{"object", "list"},
                      {"data", json::array({{{"id", rt.encoder.modelId},
                                             {"object", "model"},
                                             {"owned_by", "local"}}})}});
    });

    auto embed = [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded())
            return setJson(res, {{"error", {{"message", "bad json"}}}}, 400);
        std::string text;
        if (body["input"].is_string())
            text = body["input"].get<std::string>();
        else if (body["input"].is_array() && !body["input"].empty() && body["input"][0].is_string())
            text = body["input"][0].get<std::string>();
        else
            return setJson(res, {{"error", {{"message", "input must be string"}}}}, 400);
        auto vector = rt.encoder.embed(text);
        setJson(res, {{"object", "list"},
                      {"data", json::array({{{"object", "embedding"}, {"index", 0}, {"embedding", vector}}})},
                      {"model", body.value("model", rt.encoder.modelId)},
                      {"dim", rt.encoder.dimensions}});
    };
    svr.Post("/v1/embed", embed);
    svr.Post("/v1/embeddings", embed);

    svr.Post("/v1/search", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded())
            return setJson(res, {{"error", {{"message", "bad json"}}}}, 400);
        std::size_t k = body.value("k", 8);
        std::vector<std::pair<Doc, float>> hits;
        if (body.contains("vector") && body["vector"].is_array() && !body["vector"].empty()) {
            std::vector<float> vector = body["vector"].get<std::vector<float>>();
            hits = rt.store.search(vector, k);
        } else {
            std::string text = body.value("text", "");
            hits = rt.store.search(rt.encoder.embed(text), k);
        }
        json arr = json::array();
        for (auto const& [doc, score] : hits)
            arr.push_back({{"id", doc.id}, {"text", doc.text}, {"meta", doc.meta}, {"score", score}});
        setJson(res, {{"hits", arr}});
    });

    svr.Post("/v1/upsert", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("docs"))
            return setJson(res, {{"error", {{"message", "docs required"}}}}, 400);
        std::size_t n = 0;
        for (auto const& item : body["docs"]) {
            Doc doc;
            doc.id = item.value("id", "");
            doc.text = item.value("text", "");
            doc.meta = item.value("meta", json::object());
            std::vector<float> vector;
            if (item.contains("vector") && item["vector"].is_array() && !item["vector"].empty())
                vector = item["vector"].get<std::vector<float>>();
            else
                vector = rt.encoder.embed(doc.text);
            if (error_t err = rt.store.upsert(std::move(doc), vector); err) {
                char const* msg = err.release();
                return setJson(res, {{"error", {{"message", msg ? msg : "upsert failed"}}}}, 500);
            }
            ++n;
        }
        setJson(res, {{"upserted", n}});
    });

    svr.Post("/v1/delete", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded())
            return setJson(res, {{"error", {{"message", "bad json"}}}}, 400);
        std::size_t n = 0;
        if (body.contains("ids") && body["ids"].is_array()) {
            for (auto const& id : body["ids"])
                if (id.is_string()) {
                    rt.store.remove(id.get<std::string>());
                    ++n;
                }
        }
        setJson(res, {{"deleted", n}});
    });

    auto chat = [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("messages"))
            return setJson(res, {{"error", {{"message", "messages required"}}}}, 400);

        json ctx = {{"messages", body["messages"]}, {"query", ""}};
        Pipeline pipe;
        pipe.stage({"resolve",
                    [](Runtime& r, json& c) {
                        std::string q;
                        for (auto const& m : c["messages"])
                            if (m.value("role", "") != "system")
                                q += messageText(m["content"]);
                        c["query"] = q;
                        auto matched = resolveRules(r.rules, q);
                        json arr = json::array();
                        for (auto const& rule : matched) {
                            auto body = compressSections(rule.body, q, 3);
                            arr.push_back({{"name", rule.name}, {"body", body}});
                        }
                        c["rules"] = arr;
                        return true;
                    }})
            .stage({"recall", [](Runtime& r, json& c) {
                        auto hits = r.store.search(r.encoder.embed(c["query"].get<std::string>()), 8);
                        json arr = json::array();
                        for (auto const& [doc, score] : hits)
                            arr.push_back({{"id", doc.id}, {"text", doc.text}, {"score", score}});
                        c["hits"] = arr;
                        return true;
                    }});
        pipe.run(rt, ctx);

        std::string system, user;
        for (auto const& m : body["messages"]) {
            std::string role = m.value("role", "");
            std::string content = messageText(m["content"]);
            if (role == "system") {
                if (!system.empty())
                    system.push_back('\n');
                system += content;
            } else {
                if (!user.empty())
                    user.push_back('\n');
                user += content;
            }
        }
        if (!system.empty())
            system.push_back('\n');
        system += "Local knowledge JSON follows.\n";
        system += json {{"hits", ctx["hits"]}, {"rules", ctx["rules"]}}.dump();

        std::string modelName = body.value("model", rt.encoder.modelId);
        bool stream = body.value("stream", false);
        std::uint32_t prevMax = rt.encoder.maxTokens;
        if (body.contains("max_tokens") && body["max_tokens"].is_number_unsigned())
            rt.encoder.maxTokens = body["max_tokens"].get<std::uint32_t>();
        else if (body.contains("max_tokens") && body["max_tokens"].is_number_integer())
            rt.encoder.maxTokens = static_cast<std::uint32_t>((std::max)(0, body["max_tokens"].get<int>()));
        if (rt.encoder.maxTokens == 0)
            rt.encoder.maxTokens = prevMax ? prevMax : 256;

        if (stream) {
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_chunked_content_provider(
                "text/event-stream", [&rt, system, user, modelName, prevMax](std::size_t, httplib::DataSink& sink) {
                    auto emit = [&](json const& chunk) {
                        std::string line = "data: " + chunk.dump() + "\n\n";
                        sink.write(line.data(), line.size());
                    };
                    emit({{"id", "chat-1"},
                          {"object", "chat.completion.chunk"},
                          {"model", modelName},
                          {"choices", json::array({{{"index", 0},
                                                    {"delta", {{"role", "assistant"}}},
                                                    {"finish_reason", nullptr}}})}});
                    (void)rt.encoder.chat(system, user, [&](std::string_view piece) {
                        emit({{"id", "chat-1"},
                              {"object", "chat.completion.chunk"},
                              {"model", modelName},
                              {"choices", json::array({{{"index", 0},
                                                        {"delta", {{"content", std::string(piece)}}},
                                                        {"finish_reason", nullptr}}})}});
                    });
                    rt.encoder.maxTokens = prevMax;
                    emit({{"id", "chat-1"},
                          {"object", "chat.completion.chunk"},
                          {"model", modelName},
                          {"choices",
                           json::array({{{"index", 0}, {"delta", json::object()}, {"finish_reason", "stop"}}})}});
                    std::string done = "data: [DONE]\n\n";
                    sink.write(done.data(), done.size());
                    sink.done();
                    return true;
                });
            return;
        }

        std::string text = rt.encoder.chat(system, user);
        rt.encoder.maxTokens = prevMax;
        setJson(res, {{"id", "chat-1"},
                      {"object", "chat.completion"},
                      {"model", modelName},
                      {"choices",
                       json::array({{{"index", 0},
                                     {"message", {{"role", "assistant"}, {"content", text}}},
                                     {"finish_reason", "stop"}}})}});
    };
    svr.Post("/v1/chat", chat);
    svr.Post("/v1/chat/completions", chat);

    svr.Get("/v1/rules", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        json arr = json::array();
        for (auto const& r : rt.rules)
            arr.push_back({{"name", r.name}, {"description", r.description}});
        setJson(res, {{"rules", arr}});
    });

    svr.Post("/v1/rules", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        auto body = json::parse(req.body, nullptr, false);
        std::string query = body.is_discarded() ? "" : body.value("query", "");
        auto matched = resolveRules(rt.rules, query);
        json arr = json::array();
        for (auto const& r : matched)
            arr.push_back({{"name", r.name}, {"description", r.description}, {"body", r.body}});
        setJson(res, {{"rules", arr}});
    });
}

fs::path findRoot() {
    fs::path cwd = fs::current_path();
    for (fs::path p = cwd; !p.empty(); p = p.parent_path()) {
        if (fs::is_regular_file(p / ".config" / "config.json") ||
            fs::is_regular_file(p / ".config" / "config.example.json"))
            return p;
        if (p == p.root_path())
            break;
    }
    return cwd;
}

fs::path joinRoot(fs::path const& root, std::string const& relative) {
    fs::path p(relative);
    return p.is_absolute() ? p : root / p;
}

expected_gt<Config> Config::load(fs::path const& path) {
    expected_gt<Config> out;
    if (!fs::is_regular_file(path))
        return out.failed("missing config; copy .config/config.example.json to .config/config.json");
    try {
        std::ifstream in(path);
        json j;
        in >> j;
        char const* required[] = {"gguf",   "ctx",      "gpu",     "threads", "pooling", "listen", "index",
                                  "base",   "knowledge", "rules",   "workspace", "token",  "shadow", "rate",
                                  "refill", "chat"};
        for (char const* key : required) {
            if (!j.contains(key))
                return out.failed("config missing required key");
        }
        if (!j["chat"].is_object() || !j["chat"].contains("temperature") || !j["chat"].contains("max"))
            return out.failed("config chat requires temperature and max");
        Config c;
        c.gguf = j["gguf"].get<std::string>();
        c.ctx = j["ctx"].get<std::uint32_t>();
        c.gpu = j["gpu"].get<int>();
        c.threads = j["threads"].get<int>();
        c.pooling = j["pooling"].get<std::string>();
        c.listen = j["listen"].get<std::string>();
        c.index = j["index"].get<std::string>();
        c.base = j["base"].get<std::string>();
        c.knowledge = j["knowledge"].get<std::string>();
        c.rules = j["rules"].get<std::string>();
        c.workspace = j["workspace"].get<std::string>();
        c.token = j["token"].get<std::string>();
        c.shadow = j["shadow"].get<std::size_t>();
        c.rate = j["rate"].get<std::uint32_t>();
        c.refill = j["refill"].get<double>();
        c.chat.temperature = j["chat"]["temperature"].get<float>();
        c.chat.max = j["chat"]["max"].get<std::uint32_t>();
        if (c.gguf.empty() || c.listen.empty() || c.index.empty() || c.base.empty() || c.knowledge.empty() ||
            c.rules.empty() || c.workspace.empty() || c.pooling.empty())
            return out.failed("config path or pooling must be non-empty");
        if (c.pooling != "lasttoken")
            return out.failed("pooling must be lasttoken");
        if (c.listen.find(':') == std::string::npos)
            return out.failed("listen must be host:port");
        out.result = std::move(c);
    } catch (...) {
        return out.failed("config parse failed");
    }
    return out;
}

expected_gt<Runtime> Runtime::open(fs::path const& root) {
    expected_gt<Runtime> out;
    fs::path cfgPath = root / ".config" / "config.json";
    if (!fs::is_regular_file(cfgPath))
        return out.failed("missing config; copy .config/config.example.json to .config/config.json");
    auto cfg = Config::load(cfgPath);
    if (!cfg)
        return out.failed(cfg.error.release());
    Runtime rt;
    rt.root = root;
    rt.config = std::move(cfg.result);
    fs::path gguf = joinRoot(root, rt.config.gguf);
    if (error_t err = rt.encoder.open(gguf, rt.config.ctx, rt.config.gpu, rt.config.threads,
                                      rt.config.chat.temperature, rt.config.chat.max);
        err)
        return out.failed(err.release());
    auto store = Store::make(rt.encoder.dimensions, joinRoot(root, rt.config.index),
                             joinRoot(root, rt.config.base), rt.config.shadow);
    if (!store)
        return out.failed(store.error.release());
    rt.store = std::move(store.result);
    try {
        rt.rules = loadRules(joinRoot(root, rt.config.rules));
    } catch (...) {
        return out.failed("rules load failed");
    }
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
    doc.meta = {{"path", path.result}, {"kind", "experience"}};
    auto vector = encoder.embed(doc.text);
    if (error_t err = store.upsert(std::move(doc), vector); err)
        return out.failed(err.release());
    out.result = {{"path", path.result}, {"id", id}};
    return out;
}

int runAgent(Runtime& rt, std::string const& instruction) {
    auto vector = rt.encoder.embed(instruction);
    auto hits = rt.store.search(vector, 8);
    auto matched = resolveRules(rt.rules, instruction);
    json pack = {{"hits", json::array()}, {"rules", json::array()}};
    for (auto const& [doc, score] : hits)
        pack["hits"].push_back({{"id", doc.id}, {"text", doc.text}, {"score", score}});
    for (auto const& r : matched)
        pack["rules"].push_back({{"name", r.name}, {"body", r.body}});
    std::string system = "Local knowledge JSON follows. Decide enough vs generate.\n" + pack.dump();
    std::string reply = rt.encoder.chat(system, instruction);
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
            std::fprintf(stderr, "用法: api run \"<指令>\"\n");
            return 2;
        }
        return api::runAgent(runtime.result, instruction);
    }
    std::fprintf(stderr, "用法: api serve | api run \"<指令>\"\n");
    return 2;
}
