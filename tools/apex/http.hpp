/**
 *  @file       http.hpp
 *  @brief      Edge：HTTP 网关（鉴权闸、令牌桶、Stage 流水线、MCP 挂载、metrics）。
 */
#pragma once

#include "types.hpp"

#include <httplib.h>

namespace api {

struct Runtime;

bool authOk(Runtime const& rt, httplib::Request const& req);
void setJson(httplib::Response& res, json const& body, int status = 200);
std::string messageText(json const& content);

struct Bucket {
    double capacity = 0;
    double tokens = 0;
    double refill = 1.0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    explicit Bucket(std::uint32_t cap = 0, double refillPerSec = 1.0);
    bool tryAcquire();
};

struct Stage {
    std::string name;
    std::function<bool(Runtime&, json&)> run;
};

struct Pipeline {
    std::vector<Stage> stages;
    Pipeline& stage(Stage s);
    bool run(Runtime& rt, json& ctx);
};

/** 进程级计数快照（/v1/metrics）。 */
json metricsSnapshot(Runtime const& rt);
int serve(Runtime& rt);

} // namespace api
