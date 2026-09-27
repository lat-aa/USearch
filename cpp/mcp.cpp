/**
 *  @file       mcp.cpp
 *  @brief      Streamable HTTP MCP：tools/list、tools/call 与 JSON-RPC 面。
 */

#include "api.hpp"

namespace api {

json toolDefs() {
    auto tool = [](char const* name, char const* description) {
        return json {{"name", name},
                     {"description", description},
                     {"inputSchema",
                      {{"type", "object"},
                       {"properties",
                        {{"query", {{"type", "string"}}},
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
                         {"outcome", {{"type", "string"}}}}}}}};
    };
    return json::array({
        tool("rules", "按 query 匹配本地规则并返回正文"),
        tool("catalog", "列出规则名与简述"),
        tool("rule", "按名取单条规则"),
        tool("search", "按文本做向量检索"),
        tool("upsert", "写入或覆盖一条文档"),
        tool("delete", "按 id 删除文档"),
        tool("recall", "嵌入后检索（search 的一站式别名）"),
        tool("shell", "在 workspace 执行 shell"),
        tool("test", "运行项目测试"),
        tool("status", "git status"),
        tool("commit", "git commit"),
        tool("save", "落盘 Markdown 经验并写入向量库"),
    });
}

json callTool(Runtime& rt, std::string const& name, json const& args) {
    auto textResult = [](std::string const& text, bool error = false) {
        return json {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", error}};
    };
    try {
        if (name == "rules") {
            auto matched = resolveRules(rt.rules, args.value("query", ""));
            json arr = json::array();
            for (auto const& r : matched)
                arr.push_back({{"name", r.name}, {"description", r.description}, {"body", r.body}});
            return textResult(arr.dump(2));
        }
        if (name == "catalog") {
            json arr = json::array();
            for (auto const& r : rt.rules)
                arr.push_back({{"name", r.name}, {"description", r.description}});
            return textResult(arr.dump(2));
        }
        if (name == "rule") {
            std::string want = args.value("name", "");
            for (auto const& r : rt.rules)
                if (r.name == want)
                    return textResult(
                        json {{"name", r.name}, {"description", r.description}, {"body", r.body}}.dump(2));
            return textResult("rule not found", true);
        }
        if (name == "search" || name == "recall") {
            std::string text = args.value("text", "");
            std::size_t k = args.value("k", 8);
            auto vector = rt.encoder.embed(text);
            auto hits = rt.store.search(vector, k);
            json arr = json::array();
            for (auto const& [doc, score] : hits)
                arr.push_back({{"id", doc.id}, {"text", doc.text}, {"score", score}, {"meta", doc.meta}});
            return textResult(arr.dump(2));
        }
        if (name == "upsert") {
            Doc doc;
            doc.id = args.value("id", "");
            doc.text = args.value("text", "");
            if (doc.id.empty())
                return textResult("id required", true);
            auto vector = rt.encoder.embed(doc.text);
            if (error_t err = rt.store.upsert(std::move(doc), vector); err) {
                char const* msg = err.release();
                return textResult(msg ? msg : "upsert failed", true);
            }
            return textResult("ok");
        }
        if (name == "delete") {
            if (args.contains("ids") && args["ids"].is_array()) {
                for (auto const& id : args["ids"])
                    if (id.is_string())
                        rt.store.remove(id.get<std::string>());
            } else if (args.contains("id")) {
                rt.store.remove(args["id"].get<std::string>());
            }
            return textResult("ok");
        }
        if (name == "shell")
            return textResult(rt.shell(args.value("command", "")));
        if (name == "test") {
            if (fs::exists(rt.workspace() / "CMakeLists.txt"))
                return textResult(rt.shell("ctest --test-dir build --output-on-failure"));
            return textResult(rt.shell("echo no test runner"));
        }
        if (name == "status")
            return textResult(rt.shell("git status --short"));
        if (name == "commit")
            return textResult(rt.shell("git commit -am \"" + args.value("message", "api commit") + "\""));
        if (name == "save") {
            auto exp = experienceFromJson(args);
            auto saved = rt.saveExperience(exp);
            if (!saved) {
                char const* msg = saved.error.release();
                return textResult(msg ? msg : "save failed", true);
            }
            return textResult(saved.result.dump(2));
        }
        return textResult(std::string("unknown tool: ") + name, true);
    } catch (...) {
        return textResult("tool failed", true);
    }
}

json mcpHandle(Runtime& rt, json const& req) {
    json id = req.contains("id") ? req["id"] : json(nullptr);
    std::string method = req.value("method", "");
    json params = req.contains("params") ? req["params"] : json::object();

    auto ok = [&](json result) {
        return json {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
    };
    auto err = [&](int code, std::string const& message) {
        return json {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
    };

    if (method == "initialize") {
        return ok({{"protocolVersion", "2024-11-05"},
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
        return ok(callTool(rt, name, args));
    }
    return err(-32601, "method not found: " + method);
}

} // namespace api
