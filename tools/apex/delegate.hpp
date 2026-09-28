/**
 *  @file       delegate.hpp
 *  @brief      Remote：上游 OpenAI 兼容转发（delegate 兜底）。
 */
#pragma once

#include "types.hpp"

#include <httplib.h>

namespace api {

struct Runtime;

/** blockFor(usage, reply)：可选；返回要追加到回复末尾的文本（统计块 / 输出行）。 */
std::string delegateToUpstream(Runtime& rt, json const& messages, bool responses, httplib::Response& res,
                               bool stream = false,
                               std::function<std::string(json const&, std::string const&)> const& blockFor = {});

} // namespace api
