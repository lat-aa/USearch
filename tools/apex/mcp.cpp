/**
 *  @file       mcp.cpp
 *  @brief      Streamable HTTP MCP：tools/list、tools/call 与 JSON-RPC 面。
 */

#include "mcp.hpp"
#include "api.hpp"

#include <cmath>
#include <cstdint>

namespace api {
namespace {

/** 规则统计信封：对齐 apex.mdc / stats.sh（totalRules、token 三档、entries[].id）。 */
json rulesEnvelope(Runtime& rt, Rulequery const& rq, float compression) {
    auto matched = resolveRules(rt, rq);
    // 真分词器（有模型）或 estimate 回退；tokenMode 让上层如实标注 (est)。
    auto tok = [&](std::string_view s) -> std::size_t {
        return rt.tokensReal() ? rt.encoder.countTokens(s) : estimate(s);
    };
    // 同步全文进 turn.rules（工具消费）；统计块只贴 prompt，不重复粘贴 rules
    noteRules(rt, matched);
    std::size_t total = 0;
    std::size_t naive = 0;
    for (auto const& r : rt.rules) {
        if (!r.enabled)
            continue;
        ++total;
        naive += tok(r.body) + tok(r.description) + tok(r.name);
    }
    std::size_t selected = 0;
    float semSum = 0.0f;
    for (auto const& r : matched) {
        selected += tok(r.rule.body) + tok(r.rule.description) + tok(r.rule.name);
        if (r.activation == Activation::Semantic)
            semSum += (std::max)(r.score, 0.0f);
    }
    int base = rt.decider.sectionsFor(compression);
    std::size_t optimized = 0;
    json entries = json::array();
    std::vector<std::pair<std::string, std::string>> clipped;
    clipped.reserve(matched.size());
    for (auto const& r : matched) {
        int sections = ruleSections(r.activation, r.score, semSum, base);
        std::string body = compressSections(r.rule.body, rq.task, static_cast<std::size_t>(sections));
        optimized += tok(body) + tok(r.rule.description) + tok(r.rule.name);
        if (!body.empty())
            clipped.emplace_back(r.rule.name, body);
        json item = r.toJson();
        item["id"] = r.rule.name;
        // entries[].body = 裁剪后正文（与主路径 inject 一致）；全文在 turn.rules
        item["body"] = body;
        entries.push_back(std::move(item));
    }
    // clip / rebuildPrompt.rules 用裁剪后正文（与 inject 一致）
    noteKept(rt, std::move(clipped));
    return {{"totalRules", total},
            {"matched", matched.size()},
            {"naiveTokens", naive},
            {"selectedTokens", selected},
            {"optimizedTokens", optimized},
            {"entries", std::move(entries)},
            {"tokenMode", rt.tokensReal() ? "real" : "estimate"}};
}

double roundMoney(double x) { return std::round(x * 1e6) / 1e6; }

std::string priceModelFrom(Decision const& d, std::string const& want) {
    if (want == "pro" || want == "deepseek-v4-pro")
        return "deepseek-v4-pro";
    if (want == "flash" || want == "deepseek-flash")
        return "deepseek-flash";
    if (!want.empty())
        return want;
    // 空 model：按 decide 档位映射默认上游价目表名（非客户端实模）。
    if (d.model == Model::Strong)
        return "deepseek-v4-pro";
    return "deepseek-flash";
}

/**
 * 解析本轮客户端实模：cursor-state（hook 注入）> HTTP 头 > Agent reported。
 * 禁止在此处编造选择器名；全空则返回 false。
 */
bool resolveActual(json const& args, Mcpclient const& client, std::string& actual, std::string& source) {
    auto argModel = args.value("actual_model", "");
    auto argSource = args.value("actual_model_source", "");
    if (!argModel.empty() && (argSource == "cursor-state" || argSource == "codex-config")) {
        actual = std::move(argModel);
        source = argSource;
        return true;
    }
    if (!client.actualModel.empty()) {
        actual = client.actualModel;
        source = !client.actualModelSource.empty() ? client.actualModelSource : "reported";
        return true;
    }
    if (!argModel.empty()) {
        actual = std::move(argModel);
        source = !argSource.empty() ? argSource : "reported";
        return true;
    }
    return false;
}

/**
 * 本轮栈职责（给 🔖 行）：只陈述 turn 实测，禁止用 decide.retrieval 预测冒充 ANN/回填。
 */
} // namespace

/** 平面栈实时状态：真分词器/嵌入、USearch 行数与量化、SQLite 文档数（均取实时快照）。 */
/** 取路径 basename 并去扩展名：把 config 里的 gguf 路径显示成人读模型名。 */
static std::string modelName(std::string const& path) {
    std::size_t const slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    std::size_t const dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        base = base.substr(0, dot);
    return base;
}

json stackOf(Runtime& rt, Decision const& /*d*/) {
    std::string nanbeige;
    if (rt.encoder.tokensReal()) {
        std::string const chat = modelName(rt.config.gguf);
        std::string const embed = rt.config.embed.gguf.empty()
                                      ? (chat.empty() ? std::string("未上报") : chat + "（复用 chat）")
                                      : modelName(rt.config.embed.gguf);
        nanbeige = (chat.empty() ? std::string("chat 未上报") : chat) + " · 嵌入 " + embed +
                   " dim=" + std::to_string(rt.encoder.dimensions);
    } else {
        nanbeige = "hash 回退 dim=" + std::to_string(rt.encoder.dimensions ? rt.encoder.dimensions : 1024);
    }
    Storestats const st = rt.store.stats();
    std::string const usearch = st.rows ? ("图 " + std::to_string(st.rows) + (st.quant ? " · SQ8" : " · f32")) : "空";
    std::string const sqlite = "docs " + std::to_string(st.docs);
    return {{"nanbeige", nanbeige}, {"usearch", usearch}, {"sqlite", sqlite}};
}

json toolDefs() {
    auto tool = [](char const* name, char const* description) {
        json props = {
            {"query", {{"type", "string"}}},
            {"name", {{"type", "string"}}},
            {"text", {{"type", "string"}}},
            {"id", {{"type", "string"}}},
            {"ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            {"k", {{"type", "integer"}}},
            {"command", {{"type", "string"}}},
            {"message", {{"type", "string"}}},
            {"title", {{"type", "string"}}},
            {"summary", {{"type", "string"}}},
            {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            {"commands", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            {"files", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            {"outcome", {{"type", "string"}}},
            {"task", {{"type", "string"}}},
            {"latency", {{"type", "integer"}}},
            {"hints", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            {"manual", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            {"actual_model", {{"type", "string"}}},
            {"actual_model_source", {{"type", "string"}}},
            {"model", {{"type", "string"}}},
            {"cache_hit", {{"type", "boolean"}}},
            {"peak", {{"type", "boolean"}}},
            {"output_tokens", {{"type", "integer"}}},
        };
        return json{
            {"name", name}, {"description", description}, {"inputSchema", {{"type", "object"}, {"properties", props}}}};
    };
    return json::array({
        tool("rules", "按 task/files/manual 解析规则；返回 totalRules/matched/token 三档与 entries[].id"),
        tool("catalog", "列出规则名、简述与激活元数据"),
        tool("rule", "按名取单条规则"),
        tool("search", "按文本做向量检索"),
        tool("upsert", "写入或覆盖一条文档"),
        tool("delete", "按 id 删除文档"),
        tool("recall", "嵌入后检索（search 的一站式别名）"),
        tool("decide", "路由：模型档/深度/检索/压缩/温度；回答前调用（确定性 Features→Decision）"),
        tool("observe", "沉淀仅入队 queue；Worker 后台蒸馏写 memory"),
        tool("cost", "按规则 token + decide 档位估算费用（CNY）；返回 stack（实测）与 turn（gate/saved/"
                     "corpus 全文）；actual_model 由 hook 或 X-Apex-Actual-Model 注入"),
        tool("resolve-rules", "rules 的 Codex 别名"),
        tool("list-rules", "catalog 的 Codex 别名"),
        tool("get-rule", "rule 的 Codex 别名"),
        tool("shell", "在 workspace 执行 shell"),
        tool("test", "运行项目测试"),
        tool("status", "git status"),
        tool("commit", "git commit"),
        tool("save", "经验入队（同 observe）；兼容旧名"),
    });
}

json callTool(Runtime& rt, std::string const& name, json const& args, Mcpclient const& client) {
    Turnscope scope;
    turnSnap(rt);
    auto textResult = [](std::string const& text, bool error = false) {
        return json{{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", error}};
    };
    try {
        // Codex 旧配置仍调 resolve-rules / list-rules / get-rule；与 Cursor 共用同一实现。
        std::string tool = name;
        if (tool == "resolve-rules")
            tool = "rules";
        else if (tool == "list-rules")
            tool = "catalog";
        else if (tool == "get-rule")
            tool = "rule";
        if (tool == "rules") {
            Rulequery rq = rulequeryFromJson(args);
            if (rq.task.empty())
                rq.task = args.value("query", "");
            // 默认走 decide.compression；显式 compression 可覆盖；无 task 时全保留。
            float compression = 1.0f;
            if (args.contains("compression") && args["compression"].is_number()) {
                compression = args["compression"].get<float>();
            } else if (!rq.task.empty()) {
                Decideinput in = decideinputFromJson(args);
                if (in.task.empty())
                    in.task = rq.task;
                compression = rt.decider.decide(in).compression;
            }
            return textResult(rulesEnvelope(rt, rq, compression).dump(2));
        }
        if (tool == "catalog") {
            json arr = json::array();
            for (auto const& r : rt.rules)
                arr.push_back({{"name", r.name},
                               {"description", r.description},
                               {"always", r.always},
                               {"globs", r.globs},
                               {"enabled", r.enabled}});
            return textResult(arr.dump(2));
        }
        if (tool == "rule") {
            std::string want = args.value("name", "");
            for (auto const& r : rt.rules)
                if (r.name == want)
                    return textResult(json{
                        {"name", r.name},
                        {"description", r.description},
                        {"always", r.always},
                        {"globs", r.globs},
                        {"body", r.body}}.dump(2));
            return textResult("rule not found", true);
        }
        if (tool == "search" || tool == "recall") {
            std::string text = args.value("text", "");
            std::size_t k = args.value("k", 0);
            if (k == 0) {
                Decideinput in = decideinputFromJson(args);
                if (in.task.empty())
                    in.task = text;
                if (!in.task.empty())
                    k = rt.decider.topkFor(rt.decider.decide(in).retrieval);
                else
                    k = 8;
            }
            auto vector = rt.encoder.embed(text);
            auto hits = rt.store.search(vector, k);
            json arr = json::array();
            for (auto const& [doc, score] : hits)
                arr.push_back({{"id", doc.id}, {"text", doc.text}, {"score", score}, {"meta", doc.meta}});
            return textResult(arr.dump(2));
        }
        if (tool == "observe")
            return textResult(runObserve(rt, args).dump(2));
        if (tool == "upsert") {
            Doc doc;
            doc.id = args.value("id", "");
            doc.text = args.value("text", "");
            if (doc.id.empty())
                return textResult("id required", true);
            // 真 bug 修复：默认 kind=memory —— 否则 recall/presync 的 kind=="memory" 过滤会把它当空气。
            doc.meta = {{"kind", args.value("kind", "memory")}};
            if (args.contains("meta") && args["meta"].is_object())
                for (auto const& [k, v] : args["meta"].items())
                    doc.meta[k] = v;
            auto vector = rt.encoder.embed(doc.text);
            if (error_t err = rt.store.upsert(std::move(doc), vector); err) {
                char const* msg = err.release();
                return textResult(msg ? msg : "upsert failed", true);
            }
            return textResult("ok");
        }
        if (tool == "delete") {
            if (args.contains("ids") && args["ids"].is_array()) {
                for (auto const& id : args["ids"])
                    if (id.is_string())
                        rt.store.remove(id.get<std::string>());
            } else if (args.contains("id")) {
                rt.store.remove(args["id"].get<std::string>());
            }
            return textResult("ok");
        }
        if (tool == "decide") {
            Decideinput in = decideinputFromJson(args);
            if (in.task.empty())
                in.task = args.value("query", "");
            if (in.task.empty())
                return textResult("task required", true);
            Features f = rt.decider.features(in);
            Decision d = rt.decider.decideFrom(f);
            return textResult(Deciderecord{f, d}.toJson().dump(2));
        }
        if (tool == "cost") {
            // 实模解析优先级见 resolveActual；计价 model 与 actual_model 分列。
            std::string actual;
            std::string source;
            if (!resolveActual(args, client, actual, source))
                return textResult("actual_model required (hook/header/arg)", true);
            bool peak = false;
            if (args.contains("peak") && args["peak"].is_boolean())
                peak = args["peak"].get<bool>();
            std::uint64_t outTok = 0;
            if (args.contains("output_tokens") && args["output_tokens"].is_number_unsigned())
                outTok = args["output_tokens"].get<std::uint64_t>();
            else if (args.contains("output_tokens") && args["output_tokens"].is_number_integer())
                outTok = static_cast<std::uint64_t>((std::max)(0, args["output_tokens"].get<int>()));

            Decideinput in = decideinputFromJson(args);
            if (in.task.empty())
                in.task = args.value("query", "");
            Features f{};
            Decision d{};
            if (!in.task.empty()) {
                f = rt.decider.features(in);
                d = rt.decider.decideFrom(f);
            } else {
                d.model = Model::Weak;
                d.depth = Depth::Shallow;
                d.retrieval = Retrieval::L0;
                d.compression = 1.0f;
                d.confidence = 0.5f;
            }

            Rulequery rq = rulequeryFromJson(args);
            if (rq.task.empty())
                rq.task = in.task;
            json stats = rulesEnvelope(rt, rq, d.compression);

            bool cacheHit = false;
            json turnJson;
            {
                Turnstats& t = activeTurn(rt);
                std::lock_guard<std::mutex> lock(t.mutex);
                t.retain = d.compression;
                t.naive = stats.value("naiveTokens", 0);
                t.picked = stats.value("selectedTokens", 0);
                t.kept = stats.value("optimizedTokens", 0);
                if (t.source == "injected" && !t.prompt.empty()) {
                    t.rebuildCorpus();
                } else {
                    t.rebuildPrompt(d);
                    t.rebuildCorpus();
                }
                if (t.cache == "L1" || t.cache == "L2")
                    cacheHit = true;
                turnJson = t.toJson();
                json const lc = rt.lastCall.toJson();
                turnJson["inTok"] = lc.value("inTok", std::uint64_t{0});
                turnJson["outTok"] = lc.value("outTok", std::uint64_t{0});
                turnJson["reply"] = lc.value("reply", "");
                turnJson["replyTrunc"] = lc.value("replyTrunc", "");
                turnJson["tokenMode"] = rt.tokensReal() ? "real" : "estimate";
            }

            std::string priced = priceModelFrom(d, args.value("model", ""));
            double inRate = 0, outRate = 0;
            priceRates(priced, cacheHit, peak, inRate, outRate);
            double naiveTok = stats.value("naiveTokens", 0.0);
            double optTok = stats.value("optimizedTokens", 0.0);
            double naiveCost = roundMoney(naiveTok / 1e6 * inRate);
            double optCost = roundMoney(optTok / 1e6 * inRate);
            double outCost = roundMoney(static_cast<double>(outTok) / 1e6 * outRate);
            json out = {{"naive_input_cost", naiveCost},
                        {"optimized_input_cost", optCost},
                        {"saved_input_cost", roundMoney(naiveCost - optCost)},
                        {"output_cost", outCost},
                        {"total_cost", roundMoney(optCost + outCost)},
                        {"cache_hit", cacheHit},
                        {"peak", peak},
                        {"actual_model", actual},
                        {"actual_model_source", source},
                        {"model", priced},
                        {"naive_tokens", stats["naiveTokens"]},
                        {"selected_tokens", stats["selectedTokens"]},
                        {"optimized_tokens", stats["optimizedTokens"]},
                        {"output_tokens", outTok},
                        {"totalRules", stats["totalRules"]},
                        {"matched", stats["matched"]},
                        {"decision", d.toJson()},
                        {"stack", stackOf(rt, d)},
                        {"turn", std::move(turnJson)}};
            return textResult(out.dump(2));
        }
        if (tool == "shell")
            return textResult(rt.shell(args.value("command", "")));
        if (tool == "test") {
            if (fs::exists(rt.workspace() / "CMakeLists.txt"))
                return textResult(rt.shell("ctest --test-dir build --output-on-failure"));
            return textResult(rt.shell("echo no test runner"));
        }
        if (tool == "status")
            return textResult(rt.shell("git status --short"));
        if (tool == "commit")
            return textResult(rt.shell("git commit -am \"" + args.value("message", "api commit") + "\""));
        if (tool == "save") {
            // 与 observe 对齐：只入队，重活交 Worker；payload 带经验字段。
            auto exp = experienceFromJson(args);
            json payload = {{"title", exp.title}, {"summary", exp.summary},   {"outcome", exp.outcome},
                            {"tags", exp.tags},   {"commands", exp.commands}, {"files", exp.files}};
            json obsArgs = {{"payload", payload}};
            if (args.contains("id"))
                obsArgs["id"] = args["id"];
            return textResult(runObserve(rt, obsArgs).dump(2));
        }
        return textResult(std::string("unknown tool: ") + name, true);
    } catch (...) {
        return textResult("tool failed", true);
    }
}

json mcpHandle(Runtime& rt, json const& req, Mcpclient const& client) {
    json id = req.contains("id") ? req["id"] : json(nullptr);
    std::string method = req.value("method", "");
    json params = req.contains("params") ? req["params"] : json::object();

    auto ok = [&](json result) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; };
    auto err = [&](int code, std::string const& message) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
    };

    if (method == "initialize") {
        std::string ver = "2024-11-05";
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string())
            ver = params["protocolVersion"].get<std::string>();
        return ok({{"protocolVersion", ver},
                   {"capabilities", {{"tools", json::object()}}},
                   {"serverInfo", {{"name", "api"}, {"version", "0.1.0"}}}});
    }
    if (method == "notifications/initialized" || method == "notifications/cancelled")
        return json::object();
    if (method == "ping")
        return ok(json::object());
    if (method == "tools/list")
        return ok({{"tools", toolDefs()}});
    if (method == "tools/call") {
        std::string name = params.value("name", "");
        json args = params.contains("arguments") ? params["arguments"] : json::object();
        return ok(callTool(rt, name, args, client));
    }
    return err(-32601, "method not found: " + method);
}

} // namespace api
