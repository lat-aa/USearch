/**
 *  @file       openai.cpp
 *  @brief      OpenAI 兼容 /v1（chat/presync/embed/route）与请求级 Turnscope。
 */
#include "openai.hpp"
#include "api.hpp"
#include "render.hpp"
#include "slim.hpp"
#include <algorithm>
#include <cstdio>
namespace api {
namespace {
static void replyText(httplib::Response& res, std::string const& id, std::string const& model,
                      std::string const& payload, bool responses) {
    if (responses)
        setJson(res, {{"id", id},
                      {"object", "response"},
                      {"status", "completed"},
                      {"model", model},
                      {"output",
                       json::array({{{"type", "message"},
                                     {"role", "assistant"},
                                     {"content", json::array({{{"type", "output_text"}, {"text", payload}}})}}})}});
    else
        setJson(res, {{"id", id},
                      {"object", "chat.completion"},
                      {"model", model},
                      {"choices", json::array({{{"index", 0},
                                                {"message", {{"role", "assistant"}, {"content", payload}}},
                                                {"finish_reason", "stop"}}})}});
}
} // namespace

void writeReply(httplib::Response& res, std::string const& id, std::string const& model, std::string const& payload,
                bool responses, bool stream) {
    if (stream) {
        res.set_content(responses ? sseResponses(id, model, payload) : sseChat(id, model, payload),
                        "text/event-stream");
        return;
    }
    replyText(res, id, model, payload, responses);
}

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
            setJson(res,
                    {{"object", "list"},
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
        setJson(res, {{"decision", d.toJson()}, {"features", Deciderecord{f, d}.toJson()}, {"rules", rules}});
    });

    auto chat = [gate, &rt](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        rt.lastUserMs.store(steadyNowMs(), std::memory_order_relaxed);
        Turnscope scope;
        auto body = json::parse(req.body, nullptr, false);
        bool responses = req.path.find("/responses") != std::string::npos;
        if (responses)
            rt.v1Responses.fetch_add(1, std::memory_order_relaxed);
        else
            rt.v1Chat.fetch_add(1, std::memory_order_relaxed);
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
        body["messages"] = normalizeMessages(body["messages"]);

        // 任务文本（非 system 消息拼接），供 L1/L2 缓存与 decide 共用。
        std::string taskQuery;
        for (auto const& m : body["messages"])
            if (m.value("role", "") != "system")
                taskQuery += messageText(m["content"]);

        std::string modelName = body.value("model", rt.encoder.modelId);
        // Codex 用 wire_api=responses 且默认 stream=true；必须回 SSE 并以 response.completed 收尾。
        bool const streamReq = body.value("stream", false);

        // L1 精确缓存：纯短路，命中即返回（不评分、不建上下文）。
        if (rt.config.cache.enableL1) {
            std::string payload, source;
            if (l1Hit(rt, l1Key(rt.policyFp, taskQuery), payload, source)) {
                rt.cacheL1.fetch_add(1, std::memory_order_relaxed);
                return writeReply(res, "cache", modelName, payload, responses, streamReq);
            }
        }
        // L2 语义缓存：USearch kind=cache + 指纹 + 相似度阈值。
        if (rt.config.cache.enableL2) {
            std::string payload, source;
            if (l2Hit(rt, taskQuery, payload, source)) {
                rt.cacheL2.fetch_add(1, std::memory_order_relaxed);
                return writeReply(res, "cache", modelName, payload, responses, streamReq);
            }
        }
        // 本地 agent 关闭 / 熔断：跳过本地，直走上游兜底。
        bool localOff =
            !rt.config.agent.enabled ||
            (rt.config.agent.enableFuse && rt.fuse.tripped(rt.config.agent.fuseFail, rt.config.agent.fuseRecover));
        if (localOff) {
            rt.agentDelegate.fetch_add(1, std::memory_order_relaxed);
            std::string up = delegateToUpstream(rt, body["messages"], responses, res, streamReq);
            if (!up.empty())
                cachePut(rt, taskQuery, up, "upstream");
            return;
        }

        json ctx = {{"messages", body["messages"]}, {"query", ""}};
        Decision route{};

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
                            arr.push_back(
                                {{"name", hit.rule.name},
                                 {"activation", activationName(hit.activation)},
                                 {"score", hit.score},
                                 {"body", compressSections(hit.rule.body, q, static_cast<std::size_t>(sections))}});
                        }
                        c["rules"] = arr;
                        return true;
                    }})
            .stage({"recall", [&](Runtime& r, json& c) {
                        std::size_t k = r.decider.topkFor(route.retrieval);
                        // 多取一截再过滤：只回灌 kind=memory 且未被版本化弃用的记忆。
                        auto hits = r.store.search(r.encoder.tryEmbed(c["query"].get<std::string>()), k * 2);
                        json arr = json::array();
                        std::size_t kept = 0;
                        for (auto const& [doc, score] : hits) {
                            if (doc.meta.value("kind", "") != "memory")
                                continue;
                            if (doc.meta.value("deprecated", false))
                                continue;
                            arr.push_back({{"id", doc.id}, {"text", doc.text}, {"score", score}});
                            if (++kept >= k)
                                break;
                        }
                        c["hits"] = arr;
                        return true;
                    }});
        pipe.run(rt, ctx);

        // 复杂任务（decide 判 Strong）直接交上游：本地 3B 在 4G 卡上 ~10s，白跑不值。
        if (rt.config.agent.skipComplex && route.model == Model::Strong) {
            rt.agentDelegate.fetch_add(1, std::memory_order_relaxed);
            std::string up = delegateToUpstream(rt, body["messages"], responses, res, streamReq);
            if (!up.empty())
                cachePut(rt, taskQuery, up, "upstream");
            return;
        }

        // 本地 CoT agent：检索上下文注入 → 标签隔离解析 + 有界工具循环 → ok / delegate
        static char const* const kAgentSys =
            "你是本地执行 agent。第一行就必须给出结果，禁止长篇推理。"
            "结果是一个用 <agent-result>…</agent-result> 包裹的 JSON："
            "{\"status\":\"ok\",\"tool_calls\":[{\"name\":\"工具名\",\"args\":{}}],\"payload\":\"最终回复\"}。"
            "payload 必须极简：最多 2 句或 60 字以内（命令 / 结论 / 短答）；"
            "凡是需要长解释、教程、多步分析的问题，一律 status=\"delegate\" 交给远端大模型。"
            "tool_calls 为空数组表示不需要工具。"
            "示例：<agent-result>{\"status\":\"ok\",\"tool_calls\":[],\"payload\":\"用 git status "
            "查看。\"}</agent-result>";

        json agentMsgs = json::array();
        agentMsgs.push_back(textMessage("system", kAgentSys));
        for (auto const& m : body["messages"])
            agentMsgs.push_back(m);
        json knowledge = {{"hits", ctx["hits"]}, {"rules", ctx["rules"]}};
        agentMsgs.push_back(
            textMessage("user", "Local context (rules + memory, reference only):\n" + knowledge.dump(2)));
        notePrompt(rt, agentMsgs);

        std::string payload;
        bool localOk = false;
        try {
            localOk = agentRun(rt, modelName, agentMsgs, payload);
        } catch (std::exception const& e) {
            std::fprintf(stderr, "api: local agent failed: %s\n", e.what());
            localOk = false;
        } catch (...) {
            std::fprintf(stderr, "api: local agent failed (unknown)\n");
            localOk = false;
        }
        if (localOk) {
            cachePut(rt, taskQuery, payload, "local");
            return writeReply(res, "agent", modelName, payload, responses, streamReq);
        }
        rt.agentDelegate.fetch_add(1, std::memory_order_relaxed);
        std::string up = delegateToUpstream(rt, body["messages"], responses, res, streamReq);
        if (!up.empty())
            cachePut(rt, taskQuery, up, "upstream");
        return;
    };
    svr.Post("/v1/chat", chat);
    svr.Post("/v1/chat/completions", chat);
    svr.Post("/v1/responses", chat);

    // /v1/presync：三端 hook 的统一前置入口（L1/L2 缓存 → 检索 → 决策 → 注入文本 + 七块）
    svr.Post("/v1/presync", [gate, &rt](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        rt.lastUserMs.store(steadyNowMs(), std::memory_order_relaxed);
        rt.presyncCalls.fetch_add(1, std::memory_order_relaxed);
        Turnscope scope;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded())
            return setJson(res, {{"error", {{"message", "bad json"}}}}, 400);

        Rulequery rq = rulequeryFromJson(body);
        std::string task = rq.task.empty() ? body.value("query", "") : rq.task;
        rq.task = task;
        std::string client = body.value("client", "");
        std::string actual = body.value("actual_model", "");

        // 1) L1 精确缓存
        json cache = {{"hit", false}, {"kind", nullptr}};
        if (rt.config.cache.enableL1 && !task.empty()) {
            std::string payload, source;
            if (l1Hit(rt, l1Key(rt.policyFp, task), payload, source))
                cache = {{"hit", true}, {"kind", "L1"}};
        }
        // 2) L2 语义缓存
        if (!cache["hit"].get<bool>() && rt.config.cache.enableL2 && !task.empty()) {
            std::string payload, source;
            if (l2Hit(rt, task, payload, source))
                cache = {{"hit", true}, {"kind", "L2"}};
        }

        // 3) 决策 + 规则 + 记忆
        Decideinput in = decideinputFromJson(body);
        if (in.task.empty())
            in.task = task;
        Decision d = rt.decider.decide(in);
        auto matched = resolveRules(rt, rq);

        int base = rt.decider.sectionsFor(d.compression);
        float semSum = 0.0f;
        for (auto const& h : matched)
            if (h.activation == Activation::Semantic)
                semSum += (std::max)(h.score, 0.0f);
        json rules = json::array();
        std::size_t total = 0, naive = 0, selected = 0, optimized = 0;
        for (auto const& r : rt.rules) {
            if (!r.enabled)
                continue;
            ++total;
            naive += estimate(r.body) + estimate(r.description) + estimate(r.name);
        }
        std::string ids;
        std::string inject;
        for (auto const& h : matched) {
            int sections = ruleSections(h.activation, h.score, semSum, base);
            std::string rb = compressSections(h.rule.body, task, static_cast<std::size_t>(sections));
            selected += estimate(h.rule.body) + estimate(h.rule.description) + estimate(h.rule.name);
            optimized += estimate(rb) + estimate(h.rule.description) + estimate(h.rule.name);
            rules.push_back({{"name", h.rule.name},
                             {"activation", activationName(h.activation)},
                             {"score", h.score},
                             {"body", rb}});
            ids += (ids.empty() ? "" : " · ");
            ids += h.rule.name;
            inject += "### " + h.rule.name + "\n" + rb + "\n\n";
        }
        json memory = json::array();
        if (!task.empty()) {
            // 不变量 2：presync 禁止阻塞 embed；忙则跳过记忆检索 fail-open。
            auto q = rt.encoder.tryEmbed(task);
            if (!q.empty()) {
                auto hits = rt.store.search(q, rt.decider.topkFor(d.retrieval));
                for (auto const& [doc, score] : hits) {
                    if (doc.meta.value("kind", "") != "memory" || doc.meta.value("deprecated", false))
                        continue;
                    memory.push_back({{"id", doc.id}, {"text", doc.text}, {"score", score}});
                    inject += "- (" + doc.id + ") " + doc.text + "\n";
                }
            }
        }

        // 4) 七块（服务端权威渲染；纯 MCP 路径无上游费用）
        char conf[16], retain[16];
        std::snprintf(conf, sizeof(conf), "%d", static_cast<int>(std::lround(d.confidence * 100.0f)));
        std::snprintf(retain, sizeof(retain), "%d", static_cast<int>(std::lround(d.compression * 100.0f)));
        std::string const depthCn = d.depth == Depth::Deep     ? "深层推理"
                                    : d.depth == Depth::Medium ? "中层推理"
                                                               : "浅层推理";
        std::string const retCn = d.retrieval == Retrieval::L3   ? "深度语义检索"
                                  : d.retrieval == Retrieval::L2 ? "语义检索"
                                  : d.retrieval == Retrieval::L1 ? "关键词检索"
                                                                 : "不做检索";
        std::string reasonCn = d.reasons.empty() ? "未上报" : d.reasons.front();
        json blockIn = {{"total", total},
                        {"matched", matched.size()},
                        {"naive", naive},
                        {"selected", selected},
                        {"optimized", optimized},
                        {"ids", ids.empty() ? "无" : ids},
                        {"route", actual},
                        {"routeNote", actual.empty() ? "建议档 · 未调/v1" : ""},
                        {"depthCn", depthCn},
                        {"retCn", retCn},
                        {"retainPct", d.compression * 100.0f + 0.5f},
                        {"confPct", d.confidence * 100.0f + 0.5f},
                        {"totalCost", 0.0},
                        {"outputCost", 0.0},
                        {"priceNote", "未调/v1"},
                        {"cacheNote", cache["hit"].get<bool>() ? "命中缓存" : "未命中缓存"},
                        {"peakNote", "空闲时段"},
                        {"reasonCn", reasonCn},
                        {"biasCn", "token 为估算值"},
                        {"corpus", ""}};
        json out = {{"cache", cache},
                    {"decision", d.toJson()},
                    {"rules", rules},
                    {"memory", memory},
                    {"inject", inject},
                    {"turn", {{"naive", naive}, {"selected", selected}, {"optimized", optimized}, {"ids", ids}}},
                    {"block", renderBlock(blockIn)},
                    {"client", client}};
        setJson(res, out);
    });

    // GET：只读目录（全部规则）；POST：按 task/files/manual 解析命中。
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
        Rulequery rq = body.is_discarded() ? Rulequery{} : rulequeryFromJson(body);
        auto matched = resolveRules(rt, rq);
        json arr = json::array();
        for (auto const& r : matched)
            arr.push_back(r.toJson());
        setJson(res, {{"rules", arr}});
    });
}
} // namespace api
