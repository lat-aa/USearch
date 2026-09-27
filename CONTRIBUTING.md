# Contribution Guide

Thank you for contributing.

## Conventions

- Branch off `main-dev` and open the PR against `main-dev`. The `main` branch follows tagged releases.
- Commit subjects start with verbs like `Fix:`, `Make:`, `Improve:`, `Add:`, `Docs:`, `Chore:`.
- Reference closed issues with `Closes #N` in the body; credit collaborators with `Co-Authored-By:` trailers.

## Before you start

Pull Git submodules (StringZilla is required for `test_cpp`; NumKong is required when `USEARCH_USE_NUMKONG=ON`):

```sh
git submodule update --init --recursive
```

## C++ / CMake

```sh
# Ubuntu example
sudo apt-get update && sudo apt-get install cmake build-essential libjemalloc-dev libomp-dev

cmake -B build_debug \
  -D CMAKE_BUILD_TYPE=Debug \
  -D USEARCH_BUILD_TEST_CPP=ON \
  -D USEARCH_BUILD_BENCH_CPP=ON
cmake --build build_debug --config Debug
ctest --test-dir build_debug --output-on-failure -L unit
```

Release / RelWithDebInfo:

```sh
cmake -B build_release \
  -D CMAKE_BUILD_TYPE=RelWithDebInfo \
  -D USEARCH_BUILD_TEST_CPP=ON \
  -D USEARCH_BUILD_BENCH_CPP=ON \
  -D USEARCH_USE_NUMKONG=ON \
  -D USEARCH_USE_OPENMP=ON
cmake --build build_release --config RelWithDebInfo
ctest --test-dir build_release --output-on-failure -L unit
```

### Top-tier memory-safety matrix

| Mode | Configure extras | Run |
|------|------------------|-----|
| ASan+UBSan | `-DUSEARCH_ENABLE_ASAN=ON -DUSEARCH_ENABLE_UBSAN=ON -DUSEARCH_SANITIZE_DEBUG=OFF` | `ctest -L unit` |
| TSan | `-DUSEARCH_ENABLE_TSAN=ON -DUSEARCH_SANITIZE_DEBUG=OFF` | `ctest -L unit` |
| Coverage | `-DUSEARCH_ENABLE_COVERAGE=ON -DUSEARCH_SANITIZE_DEBUG=OFF` | `ctest` + `lcov` |
| Fuzz | Clang + `-DUSEARCH_BUILD_FUZZ=ON` | `./fuzz_index -max_total_time=60` |
| API smoke | `-DUSEARCH_BUILD_API=ON` (+ ASan optional) | `API_BIN=./build/api ./scripts/smoke_api.sh` |

Debug builds still enable ASan+UBSan by default (`USEARCH_SANITIZE_DEBUG=ON`) when no explicit sanitizer flag is set. ASan and TSan are mutually exclusive.

CI (`.github/workflows/prerelease.yml`) gates: multi-OS RelWithDebInfo, ASan+UBSan, TSan, libFuzzer smoke, coverage artifact, cppcheck, **clang-tidy via `.clang-tidy.ci` (hard)**, API HTTP/MCP smoke under ASan.

Nightly (`.github/workflows/nightly-memory.yml`) hard gates: MSan, Valgrind (+ `cmake/valgrind.supp`), 30‑minute fuzz.

### CMake options

- `USEARCH_BUILD_TEST_CPP` — C++ unit tests (`test_cpp`)
- `USEARCH_BUILD_BENCH_CPP` — C++ benchmark (`bench_cpp`)
- `USEARCH_BUILD_API` — HTTP/MCP `api` binary
- `USEARCH_BUILD_FUZZ` — libFuzzer `fuzz_index`
- `USEARCH_ENABLE_ASAN` / `USEARCH_ENABLE_UBSAN` / `USEARCH_ENABLE_TSAN`
- `USEARCH_ENABLE_COVERAGE` — gcov / llvm coverage
- `USEARCH_SANITIZE_DEBUG` — legacy Debug ASan+UBSan when explicit sanitizers are off
- `USEARCH_USE_OPENMP` — OpenMP
- `USEARCH_USE_NUMKONG` — NumKong SIMD metrics (submodule)
- `USEARCH_USE_JEMALLOC` — jemalloc helper (optional)
- `USEARCH_INSTALL` — install headers and CMake/pkg-config files

### Linting

```sh
cppcheck --enable=warning,performance,portability --error-exitcode=1 --inline-suppr \
    --suppress=missingIncludeSystem --suppress=unusedFunction \
    -I include \
    include/usearch/index.hpp \
    include/usearch/index_dense.hpp \
    include/usearch/index_plugins.hpp

cmake -B build_tidy -D CMAKE_EXPORT_COMPILE_COMMANDS=ON -D USEARCH_BUILD_TEST_CPP=ON
clang-tidy -p build_tidy cpp/test.cpp --header-filter='include/usearch/.*'
```

Useful GDB breakpoints when debugging sanitizer builds:

- `__asan::ReportGenericError`
- `__ubsan::ScopedReport::~ScopedReport`
- `usearch_raise_runtime_error`

### Cross compilation (LLVM)

```sh
sudo apt-get install -y clang lld crossbuild-essential-arm64
export CC=clang CXX=clang++ AR=llvm-ar NM=llvm-nm RANLIB=llvm-ranlib
export TARGET_ARCH=aarch64-linux-gnu
export BUILD_ARCH=arm64

cmake -B build_artifacts \
  -D CMAKE_BUILD_TYPE=Release \
  -D CMAKE_CXX_COMPILER_TARGET=${TARGET_ARCH} \
  -D CMAKE_SYSTEM_NAME=Linux \
  -D CMAKE_SYSTEM_PROCESSOR=${BUILD_ARCH} \
  -D USEARCH_BUILD_TEST_CPP=OFF \
  -D USEARCH_BUILD_BENCH_CPP=OFF
cmake --build build_artifacts --config Release
```

## Working on NumKong

NumKong lives in the `numkong/` submodule. Prefer contributing metric / SIMD fixes upstream there, then bump the submodule pin in this repo.

## Docs

Sphinx + Doxygen for C++ headers:

```sh
pip install -r docs/requirements.txt
cd docs && doxygen conf.dox && make html
```
