/**
 *  @file       openai.hpp
 *  @brief      Edge：OpenAI 兼容 /v1 路由（responses / chat / presync / models）。
 */
#pragma once

#include "types.hpp"

#include <httplib.h>

namespace api {

struct Runtime;

/** 统一 /v1 回包：stream=true 时发 SSE（Responses/Chat），否则整包 JSON。 */
void writeReply(httplib::Response& res, std::string const& id, std::string const& model, std::string const& payload,
                bool responses, bool stream, json const& usage = json::object());

void mountOpenai(httplib::Server& svr, Runtime& rt,
                 std::function<bool(httplib::Request const&, httplib::Response&)> gate);

} // namespace api
