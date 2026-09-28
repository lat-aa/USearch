/**
 *  @file       cache.hpp
 *  @brief      ModelMemory：L1 精确 / L2 语义短路缓存（纯命中，不评分）。
 */
#pragma once

#include "render.hpp"
#include "types.hpp"

namespace api {

struct Runtime;

bool l1Hit(Runtime& rt, std::string const& key, std::string& payload, std::string& source);
bool l2Hit(Runtime& rt, std::string const& task, std::string& payload, std::string& source);
void cachePut(Runtime& rt, std::string const& task, std::string const& payload, std::string const& source);
std::string l1Key(std::string const& policyFp, std::string const& task);

} // namespace api
