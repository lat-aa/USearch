/**
 * @file stub.hpp
 * @brief apexgate 测试桩控制面（仅测试树使用）。
 */
#pragma once

#include "../../tools/apex/api.hpp"

#include <string>
#include <utility>
#include <vector>

namespace api {
void stubReset();
void stubSetHits(std::vector<std::pair<Doc, float>> hits);
void stubSetChat(std::string reply);
std::vector<std::string> const& stubAudits();
} // namespace api
