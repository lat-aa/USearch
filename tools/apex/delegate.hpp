/**
 *  @file       delegate.hpp
 *  @brief      Remote：上游 OpenAI 兼容转发（delegate 兜底）。
 */
#pragma once

#include "types.hpp"

#include <httplib.h>

namespace api {

struct Runtime;

std::string delegateToUpstream(Runtime& rt, json const& messages, bool responses, httplib::Response& res);

} // namespace api
