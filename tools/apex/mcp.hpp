/**
 *  @file       mcp.hpp
 *  @brief      Edge：MCP 工具定义与调用分发。
 */
#pragma once

#include "types.hpp"

namespace api {

struct Runtime;

/** MCP 请求附带的客户端元数据（来自 HTTP 头；缺省时字段为空）。 */
struct Mcpclient {
    std::string actualModel;       ///< X-Apex-Actual-Model
    std::string actualModelSource; ///< X-Apex-Actual-Model-Source
};

json toolDefs();
json callTool(Runtime& rt, std::string const& name, json const& args, Mcpclient const& client = {});
json mcpHandle(Runtime& rt, json const& req, Mcpclient const& client = {});

} // namespace api
