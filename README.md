# USearch

**更小更快的单头文件向量相似度搜索与聚类引擎**

[![GitHub](https://img.shields.io/github/stars/unum-cloud/USearch?style=flat&label=GitHub)](https://github.com/unum-cloud/USearch)
[![License](https://img.shields.io/github/license/unum-cloud/USearch)](https://github.com/unum-cloud/USearch/blob/main/LICENSE)

C++11 仅头文件的 HNSW 近似最近邻搜索，可选通过 [NumKong](https://github.com/ashvardanian/NumKong) 提供 SIMD 度量。

- 在已发布的 Intel Sapphire Rapids 基准中，建索引速度比 FAISS 快约 10×（相同算法，更轻量的设计）
- 核心为 `include/usearch/` 下的单头文件风格
- 通过 CMake 可选启用 OpenMP / NumKong / jemalloc
- 半精度与四分之一精度存储（`bf16`、float8、`i8`、`b1` 等）
- 可从磁盘内存映射大型索引（`view`）
- 支持用户自定义度量与谓词过滤
- 已用于 ClickHouse、DuckDB、ScyllaDB、TiDB、Google UniSim 等产品

[hnsw-algorithm]: https://arxiv.org/abs/1603.09320
[faster-than-faiss]: https://www.unum.cloud/blog/2023-11-07-scaling-vector-search-with-intel

## 快速开始

```cpp
#include <usearch/index_dense.hpp>

using namespace unum::usearch;

int main() {
    metric_punned_t metric(3, metric_kind_t::cos_k, scalar_kind_t::f32_k);
    index_dense_t index = index_dense_t::make(metric);
    float vec[3] = {0.2f, 0.6f, 0.4f};

    index.reserve(10);
    index.add(42, vec);
    auto matches = index.search(vec, 10);

    // matches[0].member.key, matches[0].distance
    return 0;
}
```

更多细节见 [`cpp/README.md`](cpp/README.md)。

## 构建、测试与基准

```sh
git submodule update --init --recursive   # StringZilla（测试）+ NumKong（可选 SIMD）

cmake -B build \
  -D CMAKE_BUILD_TYPE=RelWithDebInfo \
  -D USEARCH_BUILD_TEST_CPP=ON \
  -D USEARCH_BUILD_BENCH_CPP=ON \
  -D USEARCH_USE_NUMKONG=ON

cmake --build build
./build/test_cpp
```

默认 `USEARCH_USE_NUMKONG=OFF` 仍可构建并测试；启用后可使用硬件加速距离计算。

### CMake 选项

| 选项 | 默认值 | 用途 |
|--------|---------|---------|
| `USEARCH_BUILD_TEST_CPP` | ON（主工程） | 构建 `test_cpp` |
| `USEARCH_BUILD_BENCH_CPP` | ON（主工程） | 构建 `bench_cpp` |
| `USEARCH_USE_NUMKONG` | OFF | 链接 NumKong SIMD 度量 |
| `USEARCH_USE_OPENMP` | OFF | OpenMP 线程池钩子 |
| `USEARCH_USE_JEMALLOC` | OFF | jemalloc（可选分配器路径） |
| `USEARCH_INSTALL` | OFF | 安装头文件 + CMake/pkg-config |

### 作为依赖使用

**FetchContent / add_subdirectory：**

```cmake
FetchContent_Declare(usearch GIT_REPOSITORY https://github.com/unum-cloud/USearch.git)
FetchContent_MakeAvailable(usearch)
target_link_libraries(your_target PRIVATE usearch::usearch)
```

**安装 + find_package：**

```sh
cmake -B build -D USEARCH_INSTALL=ON -D USEARCH_BUILD_TEST_CPP=OFF -D USEARCH_BUILD_BENCH_CPP=OFF
cmake --build build
cmake --install build
```

**Conan：** 通过 [`conanfile.py`](conanfile.py) 提供头文件包（导出 `include/usearch/*.hpp`）。

## 序列化

```cpp
index.save("index.usearch");
index.load("index.usearch");  // 复制到内存
index.view("index.usearch");  // 内存映射
```

## 与 FAISS 对比（摘要）

| | FAISS | USearch |
|---|------:|--------:|
| 核心体积 | ~84K SLOC | ~3K SLOC 头文件 |
| 必需依赖 | BLAS、OpenMP | 无 |
| 度量 | 固定集合 | 任意 / 用户自定义 |
| ID 宽度 | 32 / 64 位 | 32 / 40 / 64 位 |

完整数据与方法见 [BENCHMARKS.md](BENCHMARKS.md) 与 [Intel 博文][faster-than-faiss]。

## 流程

政策 SoT 在 `.config/rules`（经 MCP `rules`/`gate` 投递）；SQLite 存记忆正文与 `queue`；USearch 做 ANN。工具名：`gate`、`observe`（设计别称 save_observation）、表名 `queue`。

```text
用户输入
  ↓
【客户端】Codex / Claude / Cursor
  ↓
Hook(presync)  —— 主 LLM 之前
  Cursor : beforeSubmitPrompt → HTTP gate
  Codex  : UserPromptSubmit   → HTTP gate（或直连 /v1 时由网关内 gate）
  Claude : UserPromptSubmit   → mcp_tool gate（首选）或 HTTP gate
  ↓
MCP gate（服务端统一管线）
  ├─ L1 精确缓存（键 = hash(task)+policyFingerprint）→ 命中
  ├─ L2 语义缓存（USearch meta.kind=cache + 指纹校验）→ 命中
  │     └─→ status=answered → 见「短路出口」
  └─ 未命中 → 并行：
        ├─ 记忆：USearch(kind=memory) → SQLite 回填正文
        └─ 政策：.config/rules → resolveRules（Always/Glob 硬附加；
              Semantic 参与排序）+ SQLite 仅作冲突/禁用审计
              ↓
        RRF（仅 Semantic 政策序 ⊕ 记忆序；Always/Glob 不进 RRF）
              ↓
        Nanbeige 门控（temperature=0；混合置信）
          answerConfidence = 0.45*evidence + 0.55*self
          conflicts 非空 → 禁止 answered（fail-closed）
          ├─ answered（≥threshold 如 0.85 且无冲突且 reply 非空）
          │    └─→ 短路出口
          ├─ pack（置信不足 / 信息不足）
          │    └─→ 确定性压缩证据包（token 预算）+ 可选模型 pack[]
          └─ refuse（政策冲突）→ 展示原因，不装可答

短路出口（省主 LLM）：
  Cursor : continue=false，user_message=reply
  Codex  : decision=block + reason=reply；或 /v1 本地流式结束
  Claude : continue=false + stopReason=reply（或 block+reason）

pack 出口（主 LLM 仍跑）：
  Claude/Codex : additionalContext ← pack JSON
  Cursor       : 放行回合 + apex 强制首工具 gate 取同一 pack
  ↓
主 LLM
  Cursor → 云端（不走本机 /v1）
  Codex  → 可选本机 /v1 网关，或厂商直连
  Claude → 厂商 API（pack 已注入）
  ↓
本地工具循环（含 apex：rules→decide→cost…）
  ↓
任务结束 → 模型调用 MCP observe（仅入队）
  （可选 Stop hook 提醒模型调用，不代替入队）
  ↓
SQLite 表 queue（status=pending）→ 立即返回成功
  ↓
后台 Worker
  ├─ 原子领取
  ├─ Nanbeige 强蒸馏
  ├─ 冲突检测；冲突则 meta.conflict，不晋升 L2
  ├─ 写 SQLite 记忆（SoT，meta.kind=memory）
  └─ 写 USearch 索引（失败可重试）
  ↓
用户早已看到结果；沉淀在后台完成
```



## 集成

作为嵌入式 ANN 引擎（C++）用于 ClickHouse、DuckDB、ScyllaDB、TiDB/TiFlash、YugaByte、MemGraph，以及 Google UniSim 等研究栈。

## 许可证与引用

Apache-2.0。见 [LICENSE](LICENSE) 与 [CITATION.cff](CITATION.cff)。

```bibtex
@software{Vardanian_USearch,
doi = {10.5281/zenodo.7949415},
author = {Vardanian, Ash},
title = {{USearch by Unum Cloud}},
url = {https://github.com/unum-cloud/USearch},
version = {2.26.2},
year = {2026},
}
```
