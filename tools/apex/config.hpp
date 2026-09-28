/**
 *  @file       config.hpp
 *  @brief      仓库根定位与路径拼接（Config::load 见 types.hpp，实现见 config.cpp）。
 */
#pragma once

#include "types.hpp"

namespace api {

fs::path findRoot();
fs::path joinRoot(fs::path const& root, std::string const& relative);
std::uint64_t fnv1a64(std::string_view text) noexcept;

} // namespace api
