/**
 *  @file       agent.hpp
 *  @brief      Edge：本地 agent（标签隔离解析 + 有界 MCP 工具循环）。
 */
#pragma once

#include "types.hpp"

namespace api {

struct Runtime;

bool agentRun(Runtime& rt, std::string const& modelName, json const& agentMsgs, std::string& payload);

} // namespace api
