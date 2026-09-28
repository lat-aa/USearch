/**
 *  @file       upstream.cpp
 *  @brief      上游兜底：本地 agent delegate / 解析失败时，转发到远端 OpenAI 兼容接口。
 */
#include "api.hpp"
#include "helpers.hpp"

#include <cstdlib>
#include <httplib.h>

namespace api {

std::string delegateToUpstream(Runtime& rt, json const& messages, bool responses, httplib::Response& res) {
    // 测试 seam：SMOKE_UPSTREAM_FIXTURE 设置时短路网络，返回固定文本（无 key 也可跑）。
    if (char const* uf = std::getenv("SMOKE_UPSTREAM_FIXTURE"); uf && *uf) {
        std::string reply = uf;
        if (responses)
            setJson(res, {{"id", "upstream"},
                          {"object", "response"},
                          {"status", "completed"},
                          {"model", "deepseek-chat"},
                          {"output",
                           json::array({{{"type", "message"},
                                         {"role", "assistant"},
                                         {"content", json::array({{{"type", "output_text"}, {"text", reply}}})}}})}});
        else
            setJson(res, {{"id", "upstream"},
                          {"object", "chat.completion"},
                          {"model", "deepseek-chat"},
                          {"choices",
                           json::array({{{"index", 0},
                                         {"message", {{"role", "assistant"}, {"content", reply}}},
                                         {"finish_reason", "stop"}}})}});
        return reply;
    }

    std::string base = rt.config.upstream.base;
    if (base.empty())
        base = "https://api.deepseek.com";
    std::string keyenv = rt.config.upstream.keyenv.empty() ? "DEEPSEEK_API_KEY" : rt.config.upstream.keyenv;
    char const* key = std::getenv(keyenv.c_str());
    if ((!key || !*key))
        key = std::getenv("OPENAI_API_KEY");

    // 展平 messages → system/user 两段（上游 /chat/completions）
    std::string system, user;
    for (auto const& m : messages) {
        if (!m.is_object())
            continue;
        std::string role = m.value("role", "user");
        std::string text = messageText(m.value("content", json()));
        if (role == "system") {
            if (!system.empty())
                system.push_back('\n');
            system += text;
        } else {
            if (!user.empty())
                user.push_back('\n');
            user += text;
        }
    }

    if (!key || !*key) {
        setJson(res, {{"error", {{"message", "local delegate failed and no upstream key configured"}}}}, 502);
        return {};
    }

    // 拆 scheme/host/path
    std::string rest = base;
    std::string scheme = "https";
    std::size_t sp = rest.find("://");
    if (sp != std::string::npos) {
        scheme = rest.substr(0, sp);
        rest = rest.substr(sp + 3);
    }
    std::string host = rest;
    std::string path = "/chat/completions";
    std::size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        host = rest.substr(0, slash);
        path = rest.substr(slash);
    }
    if (path.empty() || path == "/")
        path = "/chat/completions";
    else if (path.back() != '/')
        path += "/chat/completions";

    httplib::Client cli(scheme + "://" + host);
    cli.set_connection_timeout(30);
    cli.set_read_timeout(600);

    json msgs = json::array();
    if (!system.empty())
        msgs.push_back(textMessage("system", system));
    msgs.push_back(textMessage("user", user.empty() ? " " : user));
    json body = {{"model", "deepseek-chat"}, {"messages", std::move(msgs)}, {"stream", false}};

    auto up = cli.Post(path,
                       {{"Authorization", std::string("Bearer ") + key}, {"Content-Type", "application/json"}},
                       body.dump(), "application/json");
    if (!up) {
        setJson(res, {{"error", {{"message", "upstream unreachable"}}}}, 502);
        return {};
    }
    if (up->status != 200) {
        res.status = up->status;
        res.set_content(up->body, "application/json");
        return {};
    }

    std::string reply;
    try {
        auto j = json::parse(up->body);
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty())
            reply = j["choices"][0].value("message", json::object()).value("content", "");
    } catch (...) {
        reply.clear();
    }
    if (reply.empty())
        reply = up->body;

    if (responses)
        setJson(res, {{"id", "upstream"},
                      {"object", "response"},
                      {"status", "completed"},
                      {"model", "deepseek-chat"},
                      {"output",
                       json::array({{{"type", "message"},
                                     {"role", "assistant"},
                                     {"content", json::array({{{"type", "output_text"}, {"text", reply}}})}}})}});
    else
        setJson(res, {{"id", "upstream"},
                      {"object", "chat.completion"},
                      {"model", "deepseek-chat"},
                      {"choices",
                       json::array({{{"index", 0},
                                     {"message", {{"role", "assistant"}, {"content", reply}}},
                                     {"finish_reason", "stop"}}})}});
    return reply;
}

} // namespace api