/**
 *  @file       http.cpp
 *  @brief      HTTP 网关：鉴权闸、令牌桶、Stage 流水线、MCP 挂载；OpenAI /v1 见 api.cpp。
 */

#include "http.hpp"
#include "api.hpp"
#include "render.hpp"

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

std::string messageText(json const& content) { return textOf(content); }

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

json metricsSnapshot(Runtime const& rt) {
    auto n = rt.encoder.lockWaitN.load(std::memory_order_relaxed);
    auto sum = rt.encoder.lockWaitSumMs.load(std::memory_order_relaxed);
    auto mx = rt.encoder.lockWaitMaxMs.load(std::memory_order_relaxed);
    double avg = n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
    return {{"edge",
             {{"v1Chat", rt.v1Chat.load(std::memory_order_relaxed)},
              {"v1Responses", rt.v1Responses.load(std::memory_order_relaxed)},
              {"presync", rt.presyncCalls.load(std::memory_order_relaxed)},
              {"observe", rt.observeCalls.load(std::memory_order_relaxed)}}},
            {"policy", {{"rules", rt.rules.size()}}},
            {"encode",
             {{"chatBusy", rt.encoder.chatBusy.load(std::memory_order_relaxed)},
              {"dedicatedEmbed", rt.encoder.dedicatedEmbed()},
              {"lockWaitN", n},
              {"lockWaitAvgMs", avg},
              {"lockWaitMaxMs", mx},
              {"stealChat", rt.encoder.stealChat.load(std::memory_order_relaxed)}}},
            {"async", {{"yieldSkip", rt.yieldSkip.load(std::memory_order_relaxed)}, {"queueAge", 0}}},
            {"cache",
             {{"l1", rt.cacheL1.load(std::memory_order_relaxed)}, {"l2", rt.cacheL2.load(std::memory_order_relaxed)}}}};
}

int serve(Runtime& rt) {
    auto colon = rt.config.listen.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= rt.config.listen.size()) {
        std::fprintf(stderr, "api: listen 必须为 host:port\n");
        return 1;
    }
    std::string host = rt.config.listen.substr(0, colon);
    char* portEnd = nullptr;
    long const portParsed = std::strtol(rt.config.listen.c_str() + colon + 1, &portEnd, 10);
    bool const portOk =
        portEnd != rt.config.listen.c_str() + colon + 1 && *portEnd == '\0' && portParsed > 0 && portParsed <= 65535;
    if (host.empty() || !portOk) {
        std::fprintf(stderr, "api: listen 端口无效\n");
        return 1;
    }
    int const port = static_cast<int>(portParsed);

    httplib::Server svr;
    // 默认任务队列；自定义 ThreadPool(1) 在 keep-alive 下易踩死锁/崩溃。
    Bucket bucket(rt.config.rate, rt.config.refill);
    std::mutex bucketMutex;

    std::function<bool(httplib::Request const&, httplib::Response&)> gate = [&](httplib::Request const& req,
                                                                                httplib::Response& res) {
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
        Storestats const st = rt.store.stats();
        setJson(res, {{"ok", true},
                      {"model", rt.encoder.modelReady},
                      {"dim", rt.encoder.dimensions},
                      {"tokenMode", rt.tokensReal() ? "real" : "estimate"},
                      {"docs", st.docs},
                      {"rules", rt.rules.size()},
                      {"shadow", st.quant},
                      {"v1",
                       {{"responses", rt.v1Responses.load(std::memory_order_relaxed)},
                        {"chat", rt.v1Chat.load(std::memory_order_relaxed)},
                        {"injected", rt.promptInjected.load(std::memory_order_relaxed)},
                        {"hooks",
                         {{"presync", rt.presyncCalls.load(std::memory_order_relaxed)},
                          {"observe", rt.observeCalls.load(std::memory_order_relaxed)}}},
                        {"agent",
                         {{"ok", rt.agentOk.load(std::memory_order_relaxed)},
                          {"delegate", rt.agentDelegate.load(std::memory_order_relaxed)},
                          {"parsefail", rt.agentParsefail.load(std::memory_order_relaxed)},
                          {"rounds", rt.agentRounds.load(std::memory_order_relaxed)},
                          {"cache_l1", rt.cacheL1.load(std::memory_order_relaxed)},
                          {"cache_l2", rt.cacheL2.load(std::memory_order_relaxed)},
                          {"upstream", rt.upstreamCalls.load(std::memory_order_relaxed)},
                          {"fuse", rt.fuse.openedAtMs.load(std::memory_order_relaxed) != 0},
                          {"chatBusy", rt.encoder.chatBusy.load(std::memory_order_relaxed)}}},
                        {"local",
                         {{"chats", rt.encoder.chatCalls.load(std::memory_order_relaxed)},
                          {"distills", rt.encoder.distillCalls.load(std::memory_order_relaxed)},
                          {"embeds", rt.encoder.embedCalls.load(std::memory_order_relaxed)},
                          {"searches", rt.store.searches.load(std::memory_order_relaxed)},
                          {"hits", rt.store.hits.load(std::memory_order_relaxed)}}}}}});
    });

    svr.Get("/v1/metrics", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        setJson(res, metricsSnapshot(rt));
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
                    {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}});
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

    // Streamable HTTP MCP：DELETE 终止会话（Codex/Cursor 收尾调用）。
    // 实现里无按会话状态，回 204 即"已删除"；缺此路由客户端会记 404 ERROR。
    svr.Delete("/mcp", [&](httplib::Request const& req, httplib::Response& res) {
        if (!gate(req, res))
            return;
        res.status = 204;
        res.set_content("", "text/plain");
    });

    // 先 bind 再起 Worker，避免与 listen 抢启动期资源；失败则不入队消费。
    if (!svr.bind_to_port(host.c_str(), port)) {
        std::fprintf(stderr, "api: bind 失败 %s:%d\n", host.c_str(), port);
        return 1;
    }
    rt.startWorker();
    std::printf("api 监听 http://%s:%d\n", host.c_str(), port);
    std::fflush(stdout);
    svr.listen_after_bind();
    rt.stopWorker();
    return 0;
}

} // namespace api
