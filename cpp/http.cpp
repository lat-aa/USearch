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
    // 沉淀 Worker：领取 queue → Nanbeige 蒸馏 → 写 memory（用户路径不阻塞）。
    rt.startWorker();
    // 默认任务队列；自定义 ThreadPool(1) 在 keep-alive 下易踩死锁/崩溃。
    Bucket bucket(rt.config.rate, rt.config.refill);
    std::mutex bucketMutex;

    std::function<bool(httplib::Request const&, httplib::Response&)> gate =
        [&](httplib::Request const& req, httplib::Response& res) {
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

    // Streamable HTTP MCP (Cursor / Codex):
    // GET opens optional SSE for server→client. Never emit `data: {}` (invalid JSON-RPC).
    // POST carries JSON-RPC; notifications (no id) → 202.
    svr.Get("/mcp", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        auto accept = req.get_header_value("Accept");
        if (accept.find("text/event-stream") == std::string::npos) {
            res.status = 405;
            res.set_header("Allow", "POST");
            res.set_content("SSE requires Accept: text/event-stream", "text/plain");
            return;
        }
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("Mcp-Session-Id", "1");
        // Comment-only SSE frames are ignored by JSON-RPC parsers (no event/data message).
        res.set_chunked_content_provider("text/event-stream", [](std::size_t, httplib::DataSink& sink) {
            char const open[] = ": connected\n\n";
            sink.write(open, sizeof(open) - 1);
            sink.done();
            return true;
        });
    });

    svr.Post("/mcp", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            setJson(res,
                    {{"jsonrpc", "2.0"},
                     {"id", nullptr},
                     {"error", {{"code", -32700}, {"message", "parse error"}}}});
            return;
        }
        // 头里的实模作 hook/显式参数之外的回退；每请求重读，避免进程级缓存旧名。
        Mcpclient client;
        client.actualModel = req.get_header_value("X-Apex-Actual-Model");
        client.actualModelSource = req.get_header_value("X-Apex-Actual-Model-Source");
        res.set_header("Mcp-Session-Id", "1");
        // JSON-RPC notification: no id → 202, no body.
        if (!body.contains("id") || body["id"].is_null()) {
            if (body.contains("method"))
                (void)mcpHandle(rt, body, client);
            res.status = 202;
            res.set_content("", "text/plain");
            return;
        }
        auto accept = req.get_header_value("Accept");
        json out = mcpHandle(rt, body, client);
        // Prefer JSON; wrap as SSE only when client asks for stream and not JSON.
        if (accept.find("text/event-stream") != std::string::npos &&
            accept.find("application/json") == std::string::npos) {
            res.set_header("Cache-Control", "no-cache");
            std::string frame = "event: message\ndata: " + out.dump() + "\n\n";
            res.set_content(frame, "text/event-stream");
            return;
        }
        setJson(res, out);
    });

    std::printf("api 监听 http://%s:%d\n", host.c_str(), port);
    if (!svr.listen(host, port)) {
        std::fprintf(stderr, "api: 监听失败\n");
        rt.stopWorker();
        return 1;
    }
    rt.stopWorker();
    return 0;
}

} // namespace api
