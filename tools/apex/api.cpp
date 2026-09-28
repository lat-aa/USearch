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
#include "slim.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include <toml++/toml.hpp>

namespace api {
namespace {

/** Codex Responses `input` → chat `messages`；缺 role 的片段当 user。 */
json messagesFromResponses(json const& body) {
    json messages = json::array();
    if (body.contains("instructions") && body["instructions"].is_string()) {
        std::string ins = body["instructions"].get<std::string>();
        if (!ins.empty())
            messages.push_back({{"role", "system"}, {"content", ins}});
    }
    json input = body.contains("input") ? body["input"] : json();
    auto push = [&](std::string role, json const& content) {
        if (role.empty())
            role = "user";
        messages.push_back({{"role", std::move(role)}, {"content", content}});
    };
    if (input.is_string()) {
        push("user", input.get<std::string>());
        return messages;
    }
    if (!input.is_array())
        return messages;
    for (auto const& item : input) {
        if (item.is_string()) {
            push("user", item.get<std::string>());
            continue;
        }
        if (!item.is_object())
            continue;
        std::string type = item.value("type", "");
        if (type == "function_call_output")
            continue;
        std::string role = item.value("role", type == "message" ? "user" : "");
        if (item.contains("content"))
            push(role, item["content"]);
        else if (item.contains("text") && item["text"].is_string())
            push(role.empty() ? "user" : role, item["text"].get<std::string>());
    }
    return messages;
}

} // namespace

void mountOpenai(httplib::Server& svr, Runtime& rt,
                 std::function<bool(httplib::Request const&, httplib::Response&)> gate) {
    // gate must outlive handlers: take by value into each lambda (see http.cpp stored std::function).
    svr.Get("/v1/models", [gate, &rt](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        json data = json::array();
        auto add = [&](std::string const& id) {
            data.push_back({{"id", id}, {"object", "model"}, {"owned_by", "local"}});
        };
        add(rt.encoder.modelId);
        // Codex catalog 要看到网关档位名，不能只回本地 GGUF id。
        add("deepseek-flash");
        add("deepseek-v4-pro");
        setJson(res, {{"object", "list"}, {"data", data}});
    });

    auto embed = [gate, &rt](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        try {
            auto body = json::parse(req.body, nullptr, false);
            if (body.is_discarded())
                return setJson(res, {{"error", {{"message", "bad json"}}}}, 400);
            std::string text;
            if (body.contains("input") && body["input"].is_string())
                text = body["input"].get<std::string>();
            else if (body.contains("input") && body["input"].is_array() && !body["input"].empty() &&
                     body["input"][0].is_string())
                text = body["input"][0].get<std::string>();
            else
                return setJson(res, {{"error", {{"message", "input must be string"}}}}, 400);
            auto vector = rt.encoder.embed(text);
            json emb = json::array();
            for (float v : vector)
                emb.push_back(v);
            setJson(res, {{"object", "list"},
                          {"data", json::array({{{"object", "embedding"}, {"index", 0}, {"embedding", std::move(emb)}}})},
                          {"model", body.value("model", rt.encoder.modelId)},
                          {"dim", rt.encoder.dimensions ? rt.encoder.dimensions : vector.size()}});
        } catch (std::exception const& ex) {
            setJson(res, {{"error", {{"message", ex.what()}}}}, 500);
        } catch (...) {
            setJson(res, {{"error", {{"message", "embed failed"}}}}, 500);
        }
    };
    svr.Post("/v1/embed", embed);
    svr.Post("/v1/embeddings", embed);

    svr.Post("/v1/search", [gate, &rt](httplib::Request const& req, httplib::Response& res) {
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

    svr.Post("/v1/upsert", [gate, &rt](httplib::Request const& req, httplib::Response& res) {
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

    svr.Post("/v1/delete", [gate, &rt](httplib::Request const& req, httplib::Response& res) {
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

    // 前置门控：与 MCP gate 同一管线（answered 时可省上游主 LLM）。
    svr.Post("/v1/gate", [gate, &rt](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        // 固定 schema：轻量抽取，避免 nlohmann 整树 DOM
        Slimargs slim = parseSlim(req.body);
        if (!slim.ok)
            return setJson(res, {{"error", {{"message", "bad json"}}}}, 400);
        try {
            setJson(res, runGate(rt, slimToJson(slim)));
        } catch (std::exception const& ex) {
            setJson(res, {{"error", {{"message", ex.what()}}}}, 500);
        } catch (...) {
            setJson(res, {{"error", {{"message", "gate failed"}}}}, 500);
        }
    });

    // 路由面：纯决策 + 按 compression 裁剪规则；不加载/不调用 LLM。
    svr.Post("/v1/route", [gate, &rt](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        Slimargs slim = parseSlim(req.body);
        if (!slim.ok)
            return setJson(res, {{"error", {{"message", "bad json"}}}}, 400);
        json body = slimToJson(slim);
        Decideinput in = decideinputFromJson(body);
        if (in.task.empty())
            return setJson(res, {{"error", {{"message", "task required"}}}}, 400);
        Features f = rt.decider.features(in);
        Decision d = rt.decider.decideFrom(f);
        Rulequery rq;
        rq.task = in.task;
        rq.files = in.files;
        if (body.contains("manual") && body["manual"].is_array())
            for (auto const& m : body["manual"])
                if (m.is_string())
                    rq.manual.push_back(m.get<std::string>());
        auto matched = resolveRules(rt, rq);
        json rules = json::array();
        int base = rt.decider.sectionsFor(d.compression);
        float semSum = 0.0f;
        for (auto const& r : matched)
            if (r.activation == Activation::Semantic)
                semSum += (std::max)(r.score, 0.0f);
        for (auto const& r : matched) {
            int sections = ruleSections(r.activation, r.score, semSum, base);
            json item = r.toJson();
            item["body"] = compressSections(r.rule.body, in.task, static_cast<std::size_t>(sections));
            rules.push_back(std::move(item));
        }
        setJson(res, {{"decision", d.toJson()},
                      {"features", Deciderecord {f, d}.toJson()},
                      {"rules", rules}});
    });

    auto chat = [gate, &rt](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        auto body = json::parse(req.body, nullptr, false);
        bool responses = req.path.find("/responses") != std::string::npos;
        if (body.is_discarded())
            return setJson(res, {{"error", {{"message", "bad json"}}}}, 400);
        if (responses) {
            json msgs = messagesFromResponses(body);
            if (msgs.empty())
                return setJson(res, {{"error", {{"message", "input required"}}}}, 400);
            body["messages"] = std::move(msgs);
            if (body.contains("max_output_tokens") && !body.contains("max_tokens"))
                body["max_tokens"] = body["max_output_tokens"];
        }
        if (!body.contains("messages"))
            return setJson(res, {{"error", {{"message", "messages required"}}}}, 400);

        // /v1 网关内短路：gate answered 则本地直接返回，不跑完整 Pipeline+再生成。
        if (body.value("gate", true)) {
            std::string q;
            for (auto const& m : body["messages"])
                if (m.value("role", "") != "system")
                    q += messageText(m["content"]);
            json gated = runGate(rt, {{"task", q}, {"files", body.value("files", json::array())}});
            if (gated.value("status", "") == "answered") {
                std::string reply = gated.value("reply", "");
                if (responses)
                    return setJson(res, {{"id", "gate"},
                                         {"object", "response"},
                                         {"status", "completed"},
                                         {"model", body.value("model", rt.encoder.modelId)},
                                         {"output",
                                          json::array(
                                              {{{"type", "message"},
                                                {"role", "assistant"},
                                                {"content",
                                                 json::array({{{"type", "output_text"}, {"text", reply}}})}}})},
                                         {"gate", gated}});
                return setJson(res, {{"id", "gate"},
                                     {"object", "chat.completion"},
                                     {"model", body.value("model", rt.encoder.modelId)},
                                     {"choices",
                                      json::array({{{"index", 0},
                                                    {"message", {{"role", "assistant"}, {"content", reply}}},
                                                    {"finish_reason", "stop"}}})},
                                     {"gate", gated}});
            }
            // pack/refuse：把 pack 注入后续 system（主路径仍本地 Nanbeige chat）
            if (gated.contains("pack"))
                body["_gatepack"] = gated["pack"];
        }

        json ctx = {{"messages", body["messages"]}, {"query", ""}};
        Decision route {};

        Pipeline pipe;
        // decide → resolve → recall：档位驱动温度 / 规则段数 / 检索 k（见 decide.cpp）。
        pipe.stage({"decide",
                    [&](Runtime& r, json& c) {
                        std::string q;
                        for (auto const& m : c["messages"])
                            if (m.value("role", "") != "system")
                                q += messageText(m["content"]);
                        c["query"] = q;
                        json hintBody = {{"task", q},
                                         {"files", body.value("files", json::array())},
                                         {"hints", body.value("hints", json::array())}};
                        if (body.contains("latency"))
                            hintBody["latency"] = body["latency"];
                        Decideinput in = decideinputFromJson(hintBody);
                        if (in.task.empty())
                            in.task = q;
                        route = r.decider.decide(in);
                        c["decision"] = route.toJson();
                        return true;
                    }})
            .stage({"resolve",
                    [&](Runtime& r, json& c) {
                        std::string q = c.value("query", "");
                        Rulequery rq = rulequeryFromJson(body);
                        if (rq.task.empty())
                            rq.task = q;
                        auto matched = resolveRules(r, rq);
                        json arr = json::array();
                        int base = r.decider.sectionsFor(route.compression);
                        float semSum = 0.0f;
                        for (auto const& hit : matched)
                            if (hit.activation == Activation::Semantic)
                                semSum += (std::max)(hit.score, 0.0f);
                        for (auto const& hit : matched) {
                            int sections = ruleSections(hit.activation, hit.score, semSum, base);
                            arr.push_back({{"name", hit.rule.name},
                                           {"activation", activationName(hit.activation)},
                                           {"score", hit.score},
                                           {"body", compressSections(hit.rule.body, q,
                                                                     static_cast<std::size_t>(sections))}});
                        }
                        c["rules"] = arr;
                        return true;
                    }})
            .stage({"recall",
                    [&](Runtime& r, json& c) {
                        std::size_t k = r.decider.topkFor(route.retrieval);
                        auto hits = r.store.search(r.encoder.embed(c["query"].get<std::string>()), k);
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
        // 主路径命令包：与 turn.prompt / corpus ## prompt 同源，供统计块原样展示
        std::string packPrompt = "Local knowledge JSON follows.\n";
        json knowledge = {{"hits", ctx["hits"]}, {"rules", ctx["rules"]}, {"decision", ctx["decision"]}};
        if (body.contains("_gatepack"))
            knowledge["pack"] = body["_gatepack"];
        packPrompt += knowledge.dump(2);
        system += packPrompt;
        notePrompt(rt, packPrompt);

        std::string modelName = body.value("model", rt.encoder.modelId);
        bool stream = body.value("stream", false);
        float prevTemp = rt.encoder.temperature;
        std::uint32_t prevMax = rt.encoder.maxTokens;
        if (body.contains("temperature") && body["temperature"].is_number())
            rt.encoder.temperature = body["temperature"].get<float>();
        else
            rt.encoder.temperature = route.temperature;
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
                "text/event-stream",
                [&rt, system, user, modelName, prevMax, prevTemp, responses](std::size_t, httplib::DataSink& sink) {
                    auto emit = [&](json const& chunk) {
                        std::string line = "data: " + chunk.dump() + "\n\n";
                        sink.write(line.data(), line.size());
                    };
                    auto emitEvent = [&](char const* ev, json const& chunk) {
                        std::string line = std::string("event: ") + ev + "\ndata: " + chunk.dump() + "\n\n";
                        sink.write(line.data(), line.size());
                    };
                    if (responses) {
                        // Codex wire_api=responses：事件名必须是 response.*，不能冒充 chat.completion。
                        json created = {{"id", "resp-1"},
                                        {"object", "response"},
                                        {"status", "in_progress"},
                                        {"model", modelName},
                                        {"output", json::array()}};
                        emitEvent("response.created", {{"type", "response.created"}, {"response", created}});
                        std::string acc;
                        (void)rt.encoder.chat(system, user, [&](std::string_view piece) {
                            acc.append(piece.data(), piece.size());
                            emitEvent("response.output_text.delta",
                                      {{"type", "response.output_text.delta"},
                                       {"delta", std::string(piece)}});
                        });
                        rt.encoder.maxTokens = prevMax;
                        rt.encoder.temperature = prevTemp;
                        json done = {{"id", "resp-1"},
                                     {"object", "response"},
                                     {"status", "completed"},
                                     {"model", modelName},
                                     {"output",
                                      json::array({{{"type", "message"},
                                                    {"role", "assistant"},
                                                    {"content",
                                                     json::array({{{"type", "output_text"}, {"text", acc}}})}}})}};
                        emitEvent("response.completed", {{"type", "response.completed"}, {"response", done}});
                        sink.done();
                        return true;
                    }
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
                    rt.encoder.temperature = prevTemp;
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
        rt.encoder.temperature = prevTemp;
        if (responses) {
            setJson(res, {{"id", "resp-1"},
                          {"object", "response"},
                          {"status", "completed"},
                          {"model", modelName},
                          {"decision", route.toJson()},
                          {"output",
                           json::array({{{"type", "message"},
                                         {"role", "assistant"},
                                         {"content",
                                          json::array({{{"type", "output_text"}, {"text", text}}})}}})}});
            return;
        }
        setJson(res, {{"id", "chat-1"},
                      {"object", "chat.completion"},
                      {"model", modelName},
                      {"decision", route.toJson()},
                      {"choices",
                       json::array({{{"index", 0},
                                     {"message", {{"role", "assistant"}, {"content", text}}},
                                     {"finish_reason", "stop"}}})}});
    };
    svr.Post("/v1/chat", chat);
    svr.Post("/v1/chat/completions", chat);
    svr.Post("/v1/responses", chat);

    svr.Get("/v1/rules", [gate, &rt](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        json arr = json::array();
        for (auto const& r : rt.rules)
            arr.push_back({{"name", r.name},
                           {"description", r.description},
                           {"always", r.always},
                           {"globs", r.globs},
                           {"enabled", r.enabled}});
        setJson(res, {{"rules", arr}});
    });

    svr.Post("/v1/rules", [gate, &rt](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        auto body = json::parse(req.body, nullptr, false);
        Rulequery rq = body.is_discarded() ? Rulequery {} : rulequeryFromJson(body);
        auto matched = resolveRules(rt, rq);
        json arr = json::array();
        for (auto const& r : matched)
            arr.push_back(r.toJson());
        setJson(res, {{"rules", arr}});
    });
}

fs::path findRoot() {
    fs::path cwd = fs::current_path();
    for (fs::path p = cwd; !p.empty(); p = p.parent_path()) {
        if (fs::is_regular_file(p / ".config" / "config.toml") ||
            fs::is_regular_file(p / ".config" / "config.example.toml"))
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

namespace {

char const* kMissingToml = "missing config; copy .config/config.example.toml to .config/config.toml";

bool asString(toml::node_view<toml::node> n, std::string& out) {
    auto* s = n.as_string();
    if (!s)
        return false;
    out = std::string{s->get()};
    return true;
}

bool asI64(toml::node_view<toml::node> n, std::int64_t& out) {
    auto* v = n.as_integer();
    if (!v)
        return false;
    out = v->get();
    return true;
}

bool asF64(toml::node_view<toml::node> n, double& out) {
    if (auto* f = n.as_floating_point()) {
        out = f->get();
        return true;
    }
    // TOML 允许整数写在浮点位（如 refill = 1）
    if (auto* i = n.as_integer()) {
        out = static_cast<double>(i->get());
        return true;
    }
    return false;
}

} // namespace

expected_gt<Config> Config::load(fs::path const& path) {
    expected_gt<Config> out;
    if (!fs::is_regular_file(path))
        return out.failed(kMissingToml);
    toml::table root;
    try {
        root = toml::parse_file(path.string());
    } catch (toml::parse_error const& err) {
        return out.failed(std::string(err.description()).c_str());
    } catch (...) {
        return out.failed("config parse failed");
    }

    char const* required[] = {"gguf",   "ctx",      "gpu",     "threads", "pooling", "listen", "index",
                              "base",   "knowledge", "rules",   "workspace", "token",  "shadow", "rate",
                              "refill", "chat",     "decide",  "gate"};
    for (char const* key : required) {
        if (!root.contains(key))
            return out.failed("config missing required key");
    }

    auto* chatTbl = root["chat"].as_table();
    if (!chatTbl || !chatTbl->contains("temperature") || !chatTbl->contains("max"))
        return out.failed("config chat requires temperature and max");
    auto* decideTbl = root["decide"].as_table();
    if (!decideTbl)
        return out.failed("config decide must be table");
    char const* decideKeys[] = {"speed",    "high",     "low",      "margin",  "templow", "tempmid",
                                "temphigh", "tempcap",  "wmedium",  "wcomplex", "maxtask", "maxfile",
                                "keeplow",  "keepmid",  "keephigh", "topk",    "lexicon"};
    for (char const* key : decideKeys) {
        if (!decideTbl->contains(key))
            return out.failed("config decide missing required key");
    }
    auto* topkArr = (*decideTbl)["topk"].as_array();
    if (!topkArr || topkArr->size() != 4)
        return out.failed("config decide.topk must be array of 4");

    Config c;
    auto failType = [&](char const* msg) -> expected_gt<Config> {
        expected_gt<Config> e;
        return e.failed(msg);
    };
    std::int64_t i64 = 0;
    double f64 = 0.0;

    if (!asString(root["gguf"], c.gguf))
        return failType("gguf must be string");
    if (!asI64(root["ctx"], i64) || i64 < 0)
        return failType("ctx must be unsigned integer");
    c.ctx = static_cast<std::uint32_t>(i64);
    if (!asI64(root["gpu"], i64))
        return failType("gpu must be integer");
    c.gpu = static_cast<int>(i64);
    if (!asI64(root["threads"], i64))
        return failType("threads must be integer");
    c.threads = static_cast<int>(i64);
    if (!asString(root["pooling"], c.pooling))
        return failType("pooling must be string");
    if (!asString(root["listen"], c.listen))
        return failType("listen must be string");
    if (!asString(root["index"], c.index))
        return failType("index must be string");
    if (!asString(root["base"], c.base))
        return failType("base must be string");
    if (!asString(root["knowledge"], c.knowledge))
        return failType("knowledge must be string");
    if (!asString(root["rules"], c.rules))
        return failType("rules must be string");
    if (!asString(root["workspace"], c.workspace))
        return failType("workspace must be string");
    if (!asString(root["token"], c.token))
        return failType("token must be string");
    if (!asI64(root["shadow"], i64) || i64 < 0)
        return failType("shadow must be unsigned integer");
    c.shadow = static_cast<std::size_t>(i64);
    if (!asI64(root["rate"], i64) || i64 < 0)
        return failType("rate must be unsigned integer");
    c.rate = static_cast<std::uint32_t>(i64);
    if (!asF64(root["refill"], f64))
        return failType("refill must be number");
    c.refill = f64;

    if (!asF64((*chatTbl)["temperature"], f64))
        return failType("chat.temperature must be number");
    c.chat.temperature = static_cast<float>(f64);
    if (!asI64((*chatTbl)["max"], i64) || i64 < 0)
        return failType("chat.max must be unsigned integer");
    c.chat.max = static_cast<std::uint32_t>(i64);

    toml::table& d = *decideTbl;
    if (!asI64(d["speed"], i64) || i64 < 0)
        return failType("decide.speed must be unsigned integer");
    c.decide.speed = static_cast<std::uint64_t>(i64);
    if (!asI64(d["high"], i64) || i64 < 0 || i64 > 100)
        return failType("decide.high must be 0..100");
    c.decide.high = static_cast<std::uint8_t>(i64);
    if (!asI64(d["low"], i64) || i64 < 0 || i64 > 100)
        return failType("decide.low must be 0..100");
    c.decide.low = static_cast<std::uint8_t>(i64);
    if (!asI64(d["margin"], i64) || i64 < 0 || i64 > 100)
        return failType("decide.margin must be 0..100");
    c.decide.margin = static_cast<std::uint8_t>(i64);
    if (!asF64(d["templow"], f64))
        return failType("decide.templow must be number");
    c.decide.temperature.low = static_cast<float>(f64);
    if (!asF64(d["tempmid"], f64))
        return failType("decide.tempmid must be number");
    c.decide.temperature.mid = static_cast<float>(f64);
    if (!asF64(d["temphigh"], f64))
        return failType("decide.temphigh must be number");
    c.decide.temperature.high = static_cast<float>(f64);
    if (!asF64(d["tempcap"], f64))
        return failType("decide.tempcap must be number");
    c.decide.temperature.cap = static_cast<float>(f64);
    if (!asI64(d["wmedium"], i64) || i64 < 0 || i64 > 100)
        return failType("decide.wmedium must be 0..100");
    c.decide.wmedium = static_cast<std::uint8_t>(i64);
    if (!asI64(d["wcomplex"], i64) || i64 < 0 || i64 > 100)
        return failType("decide.wcomplex must be 0..100");
    c.decide.wcomplex = static_cast<std::uint8_t>(i64);
    if (!asI64(d["maxtask"], i64) || i64 <= 0)
        return failType("decide.maxtask must be > 0");
    c.decide.maxtask = static_cast<std::size_t>(i64);
    if (!asI64(d["maxfile"], i64) || i64 <= 0)
        return failType("decide.maxfile must be > 0");
    c.decide.maxfile = static_cast<std::size_t>(i64);
    if (!asF64(d["keeplow"], f64))
        return failType("decide.keeplow must be number");
    c.decide.keeplow = static_cast<float>(f64);
    if (!asF64(d["keepmid"], f64))
        return failType("decide.keepmid must be number");
    c.decide.keepmid = static_cast<float>(f64);
    if (!asF64(d["keephigh"], f64))
        return failType("decide.keephigh must be number");
    c.decide.keephigh = static_cast<float>(f64);
    for (std::size_t i = 0; i < 4; ++i) {
        auto* elem = (*topkArr)[i].as_integer();
        if (!elem || elem->get() <= 0)
            return failType("decide.topk entries must be > 0");
        c.decide.topk[i] = static_cast<std::size_t>(elem->get());
    }
    if (!asString(d["lexicon"], c.decide.lexicon))
        return failType("decide.lexicon must be string");

    auto* gateTbl = root["gate"].as_table();
    if (!gateTbl)
        return out.failed("config gate must be table");
    char const* gateKeys[] = {"threshold", "l2", "cachek", "reflect", "minevid",
                              "wevid", "wself", "packtok", "minlen"};
    for (char const* key : gateKeys) {
        if (!gateTbl->contains(key))
            return out.failed("config gate missing required key");
    }
    toml::table& g = *gateTbl;
    if (!asF64(g["threshold"], f64))
        return failType("gate.threshold must be number");
    c.gate.threshold = static_cast<float>(f64);
    if (!asF64(g["l2"], f64))
        return failType("gate.l2 must be number");
    c.gate.l2 = static_cast<float>(f64);
    if (!asI64(g["cachek"], i64) || i64 <= 0)
        return failType("gate.cachek must be > 0");
    c.gate.cachek = static_cast<std::size_t>(i64);
    if (!asI64(g["reflect"], i64) || i64 < 0 || i64 > 2)
        return failType("gate.reflect must be 0..2");
    c.gate.reflect = static_cast<int>(i64);
    if (!asF64(g["minevid"], f64))
        return failType("gate.minevid must be number");
    c.gate.minevid = static_cast<float>(f64);
    if (!asF64(g["wevid"], f64))
        return failType("gate.wevid must be number");
    c.gate.wevid = static_cast<float>(f64);
    if (!asF64(g["wself"], f64))
        return failType("gate.wself must be number");
    c.gate.wself = static_cast<float>(f64);
    if (!asI64(g["packtok"], i64) || i64 <= 0)
        return failType("gate.packtok must be > 0");
    c.gate.packtok = static_cast<std::size_t>(i64);
    if (!asI64(g["minlen"], i64) || i64 <= 0)
        return failType("gate.minlen must be > 0");
    c.gate.minlen = static_cast<std::size_t>(i64);

    if (c.gguf.empty() || c.listen.empty() || c.index.empty() || c.base.empty() || c.knowledge.empty() ||
        c.rules.empty() || c.workspace.empty() || c.pooling.empty())
        return out.failed("config path or pooling must be non-empty");
    if (c.decide.lexicon.empty())
        return out.failed("decide.lexicon path must be non-empty");
    if (c.pooling != "lasttoken")
        return out.failed("pooling must be lasttoken");
    if (c.listen.find(':') == std::string::npos)
        return out.failed("listen must be host:port");
    out.result = std::move(c);
    return out;
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
    // 政策指纹只在启动算一次；门控热路径不再每次拼规则正文。
    rt.policyFp = policyFingerprint(rt.rules);
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
    auto matched = resolveRules(rt, Rulequery {instruction, {}, {}});
    json pack = {{"hits", json::array()}, {"rules", json::array()}};
    for (auto const& [doc, score] : hits)
        pack["hits"].push_back({{"id", doc.id}, {"text", doc.text}, {"score", score}});
    for (auto const& r : matched)
        pack["rules"].push_back({{"name", r.rule.name},
                                 {"activation", activationName(r.activation)},
                                 {"body", r.rule.body}});
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
