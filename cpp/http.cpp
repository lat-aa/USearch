/**
 *  @file       http.cpp
 *  @brief      HTTP 网关：鉴权闸、令牌桶、Stage 流水线、MCP 挂载；OpenAI /v1 见 api.cpp。
 */

#include "api.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace api {

bool authOk(Runtime const& rt, httplib::Request const& req) {
    if (rt.config.token.empty())
        return true;
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end())
        return false;
    std::string const& h = it->second;
    std::string prefix = "Bearer ";
    return h.size() > prefix.size() && h.compare(0, prefix.size(), prefix) == 0 &&
           h.substr(prefix.size()) == rt.config.token;
}

void setJson(httplib::Response& res, json const& body, int status) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

std::string messageText(json const& content) {
    if (content.is_string())
        return content.get<std::string>();
    if (content.is_array()) {
        std::ostringstream ss;
        for (auto const& part : content) {
            if (part.is_string())
                ss << part.get<std::string>() << '\n';
            else if (part.contains("text"))
                ss << part["text"].get<std::string>() << '\n';
        }
        return ss.str();
    }
    return content.dump();
}

Bucket::Bucket(std::uint32_t cap, double refillPerSec)
    : capacity(static_cast<double>(cap)), tokens(static_cast<double>(cap)), refill(refillPerSec),
      last(std::chrono::steady_clock::now()) {}

bool Bucket::tryAcquire() {
    if (capacity <= 0.0)
        return true;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last).count();
    last = now;
    tokens = (std::min)(capacity, tokens + elapsed * refill);
    if (tokens >= 1.0) {
        tokens -= 1.0;
        return true;
    }
    return false;
}

Pipeline& Pipeline::stage(Stage s) {
    stages.push_back(std::move(s));
    return *this;
}

bool Pipeline::run(Runtime& rt, json& ctx) {
    for (auto& s : stages)
        if (!s.run(rt, ctx))
            return false;
    return true;
}

int serve(Runtime& rt) {
    auto colon = rt.config.listen.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= rt.config.listen.size()) {
        std::fprintf(stderr, "api: listen 必须为 host:port\n");
        return 1;
    }
    std::string host = rt.config.listen.substr(0, colon);
    int port = std::atoi(rt.config.listen.c_str() + colon + 1);
    if (host.empty() || port <= 0 || port > 65535) {
        std::fprintf(stderr, "api: listen 端口无效\n");
        return 1;
    }

    httplib::Server svr;
    Bucket bucket(rt.config.rate, rt.config.refill);
    std::mutex bucketMutex;

    auto gate = [&](httplib::Request const& req, httplib::Response& res) {
        if (req.path == "/alive")
            return true;
        if (!authOk(rt, req)) {
            setJson(res, {{"error", {{"message", "unauthorized"}, {"type", "api"}}}}, 401);
            return false;
        }
        if (rt.config.rate > 0) {
            std::lock_guard<std::mutex> lock(bucketMutex);
            if (!bucket.tryAcquire()) {
                setJson(res, {{"error", {{"message", "rate limited"}, {"type", "api"}}}}, 429);
                return false;
            }
        }
        return true;
    };

    svr.Get("/alive", [](httplib::Request const&, httplib::Response& res) { setJson(res, {{"ok", true}}); });

    svr.Get("/ready", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        setJson(res, {{"ok", true},
                      {"model", rt.encoder.modelReady},
                      {"dim", rt.encoder.dimensions},
                      {"docs", rt.store.docs.size()},
                      {"rules", rt.rules.size()},
                      {"shadow", rt.store.quant}});
    });

    mountOpenai(svr, rt, gate);

    svr.Get("/mcp", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("mcp-session-id", "1");
        res.status = 200;
        res.set_content("event: message\ndata: {}\n\n", "text/event-stream");
    });

    svr.Post("/mcp", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded())
            return setJson(res, {{"jsonrpc", "2.0"}, {"error", {{"code", -32700}, {"message", "parse error"}}}});
        res.set_header("mcp-session-id", "1");
        if (!body.contains("id")) {
            res.status = 202;
            res.set_content("", "text/plain");
            if (body.contains("method"))
                mcpHandle(rt, body);
            return;
        }
        setJson(res, mcpHandle(rt, body));
    });

    std::printf("api 监听 http://%s:%d\n", host.c_str(), port);
    if (!svr.listen(host, port)) {
        std::fprintf(stderr, "api: 监听失败\n");
        return 1;
    }
    return 0;
}

} // namespace api
