/**
 *  @file       plugins.hpp
 *  @brief      度量/量化/硬件分发插件聚合入口。
 *
 *  对外仍 `#include <plugins/plugins.hpp>`。实现见同目录 kinds…exact。
 */
#pragma once

#include <plugins/kinds.hpp>
#include <plugins/floats.hpp>
#include <plugins/exec.hpp>
#include <plugins/alloc.hpp>
#include <plugins/casts.hpp>
#include <plugins/metrics.hpp>
#include <plugins/isa.hpp>
#include <plugins/punned.hpp>
#include <plugins/exact.hpp>