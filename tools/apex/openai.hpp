/**
 *  @file       openai.hpp
 *  @brief      Edge：OpenAI 兼容 /v1 路由（responses / chat / presync / models）。
 */
#pragma once

#include "types.hpp"

#include <httplib.h>

namespace api {

struct Runtime;

void mountOpenai(httplib::Server& svr, Runtime& rt,
                 std::function<bool(httplib::Request const&, httplib::Response&)> gate);

} // namespace api
