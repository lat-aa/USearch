# USearch

**Smaller & Faster Single-Header Similarity Search & Clustering Engine for Vectors**

[![GitHub](https://img.shields.io/github/stars/unum-cloud/USearch?style=flat&label=GitHub)](https://github.com/unum-cloud/USearch)
[![License](https://img.shields.io/github/license/unum-cloud/USearch)](https://github.com/unum-cloud/USearch/blob/main/LICENSE)

C++11 header-only HNSW approximate nearest-neighbor search, with optional SIMD metrics via [NumKong](https://github.com/ashvardanian/NumKong).

- 10× faster indexing than FAISS in published Intel Sapphire Rapids benchmarks (same algorithm, lighter design)
- Single-header style core under `include/usearch/`
- Optional OpenMP / NumKong / jemalloc via CMake
- Half- and quarter-precision storage (`bf16`, float8, `i8`, `b1`, …)
- Memory-map large indexes from disk (`view`)
- User-defined metrics and predicate filters
- Trusted in products such as ClickHouse, DuckDB, ScyllaDB, TiDB, and Google UniSim

[hnsw-algorithm]: https://arxiv.org/abs/1603.09320
[faster-than-faiss]: https://www.unum.cloud/blog/2023-11-07-scaling-vector-search-with-intel

## Quick start

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

More detail: [`cpp/README.md`](cpp/README.md).

## Build, test, and bench

```sh
git submodule update --init --recursive   # StringZilla (tests) + NumKong (optional SIMD)

cmake -B build \
  -D CMAKE_BUILD_TYPE=RelWithDebInfo \
  -D USEARCH_BUILD_TEST_CPP=ON \
  -D USEARCH_BUILD_BENCH_CPP=ON \
  -D USEARCH_USE_NUMKONG=ON

cmake --build build
./build/test_cpp
```

Default `USEARCH_USE_NUMKONG=OFF` still builds and tests; enable it for hardware-accelerated distances.

### CMake options

| Option | Default | Purpose |
|--------|---------|---------|
| `USEARCH_BUILD_TEST_CPP` | ON (main project) | Build `test_cpp` |
| `USEARCH_BUILD_BENCH_CPP` | ON (main project) | Build `bench_cpp` |
| `USEARCH_USE_NUMKONG` | OFF | Link NumKong SIMD metrics |
| `USEARCH_USE_OPENMP` | OFF | OpenMP thread pool hooks |
| `USEARCH_USE_JEMALLOC` | OFF | jemalloc (optional allocator path) |
| `USEARCH_INSTALL` | OFF | Install headers + CMake/pkg-config |

### Consume as a dependency

**FetchContent / add_subdirectory:**

```cmake
FetchContent_Declare(usearch GIT_REPOSITORY https://github.com/unum-cloud/USearch.git)
FetchContent_MakeAvailable(usearch)
target_link_libraries(your_target PRIVATE usearch::usearch)
```

**Install + find_package:**

```sh
cmake -B build -D USEARCH_INSTALL=ON -D USEARCH_BUILD_TEST_CPP=OFF -D USEARCH_BUILD_BENCH_CPP=OFF
cmake --build build
cmake --install build
```

**Conan:** header package via [`conanfile.py`](conanfile.py) (exports `include/usearch/*.hpp`).

## Serialization

```cpp
index.save("index.usearch");
index.load("index.usearch");  // copy into RAM
index.view("index.usearch");  // memory-map
```

## Comparison with FAISS (summary)

| | FAISS | USearch |
|---|------:|--------:|
| Core size | ~84K SLOC | ~3K SLOC headers |
| Required deps | BLAS, OpenMP | none |
| Metrics | fixed set | any / user-defined |
| ID width | 32 / 64-bit | 32 / 40 / 64-bit |

Full numbers and methodology: [BENCHMARKS.md](BENCHMARKS.md) and the [Intel blog][faster-than-faiss].

## Integrations

Used as an embedded ANN engine (C++) in ClickHouse, DuckDB, ScyllaDB, TiDB/TiFlash, YugaByte, MemGraph, and research stacks such as Google UniSim.

## License and citation

Apache-2.0. See [LICENSE](LICENSE) and [CITATION.cff](CITATION.cff).

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
