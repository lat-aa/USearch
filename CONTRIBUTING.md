# 贡献指南

感谢你的贡献。

## 约定

- 从 `main-dev` 拉出分支，并向 `main-dev` 提交 PR。`main` 分支跟随带标签的正式发布。
- 提交说明以动词开头，例如：`Fix:`、`Make:`、`Improve:`、`Add:`、`Docs:`、`Chore:`。
- 在正文中用 `Closes #N` 关联已关闭的议题；用 `Co-Authored-By:` trailer 标注合作者。

## 开始之前

拉取 Git 子模块（`test_cpp` 需要 StringZilla；启用 `USEARCH_USE_NUMKONG=ON` 时需要 NumKong）：

```sh
git submodule update --init --recursive
```

## C++ / CMake

```sh
# Ubuntu 示例
sudo apt-get update && sudo apt-get install cmake build-essential libjemalloc-dev libomp-dev

cmake -B build_debug \
  -D CMAKE_BUILD_TYPE=Debug \
  -D USEARCH_BUILD_TEST_CPP=ON \
  -D USEARCH_BUILD_BENCH_CPP=ON
cmake --build build_debug --config Debug
ctest --test-dir build_debug --output-on-failure -L unit
```

Release / RelWithDebInfo：

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

### 顶级内存安全矩阵

| 模式 | 额外配置 | 运行 |
|------|------------------|-----|
| ASan+UBSan | `-DUSEARCH_ENABLE_ASAN=ON -DUSEARCH_ENABLE_UBSAN=ON -DUSEARCH_SANITIZE_DEBUG=OFF` | `ctest -L unit` |
| TSan | `-DUSEARCH_ENABLE_TSAN=ON -DUSEARCH_SANITIZE_DEBUG=OFF` | `ctest -L unit` |
| Coverage | `-DUSEARCH_ENABLE_COVERAGE=ON -DUSEARCH_SANITIZE_DEBUG=OFF` | `ctest` + `lcov` |
| Fuzz | Clang + `-DUSEARCH_BUILD_FUZZ=ON` | `./fuzz_index -max_total_time=60` |
| API smoke | `-DUSEARCH_BUILD_API=ON`（可选加 ASan） | `API_BIN=./build/api ./scripts/smoke_api.sh` |

在未显式设置 sanitizer 标志时，Debug 构建默认仍启用 ASan+UBSan（`USEARCH_SANITIZE_DEBUG=ON`）。ASan 与 TSan 互斥，不可同时开启。

CI（`.github/workflows/prerelease.yml` + `quality.yml`）门禁：

| 门禁 | 工作流 |
|------|----------|
| ASan+UBSan+正确性、TSan、fuzz smoke | `quality.yml`（也为 **Release** 所要求） |
| 多操作系统单元测试（x86/ARM/macOS/Windows）、MSVC ASan | `prerelease.yml` |
| 覆盖率**下限**（`MIN_LINE_PCT`，`include/usearch` 默认 40%） | `prerelease.yml` |
| cppcheck + `.clang-tidy.ci` | `prerelease.yml` |
| ASan 下的 API HTTP/MCP | `prerelease.yml` |
| CodeQL security-and-quality | `codeql.yml` |
| PR 持续 fuzz + seed corpus | `cifuzz.yml` |
| 夜间 MSan / Valgrind / 30 分钟 fuzz | `nightly-memory.yml` |

本地正确性差分：`test_correctness`（精确检索 vs HNSW，SQ8 候选 vs f32）。
OSS-Fuzz / ClusterFuzzLite 脚手架：`ossfuzz/`、`.clusterfuzzlite/`、`fuzz/corpus/`。

### CMake 选项

- `USEARCH_BUILD_TEST_CPP` — C++ 单元测试（`test_cpp`）
- `USEARCH_BUILD_BENCH_CPP` — C++ 基准测试（`bench_cpp`）
- `USEARCH_BUILD_API` — HTTP/MCP `api` 二进制
- `USEARCH_BUILD_FUZZ` — libFuzzer `fuzz_index`
- `USEARCH_ENABLE_ASAN` / `USEARCH_ENABLE_UBSAN` / `USEARCH_ENABLE_TSAN`
- `USEARCH_ENABLE_COVERAGE` — gcov / llvm 覆盖率
- `USEARCH_SANITIZE_DEBUG` — 未显式开启 sanitizer 时，Debug 下的旧版 ASan+UBSan
- `USEARCH_USE_OPENMP` — OpenMP
- `USEARCH_USE_NUMKONG` — NumKong SIMD 度量（子模块）
- `USEARCH_USE_JEMALLOC` — jemalloc 辅助（可选）
- `USEARCH_INSTALL` — 安装头文件与 CMake/pkg-config 文件

### 静态检查

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

调试 sanitizer 构建时可用的 GDB 断点：

- `__asan::ReportGenericError`
- `__ubsan::ScopedReport::~ScopedReport`
- `usearch_raise_runtime_error`

### 交叉编译（LLVM）

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

## 参与 NumKong 开发

NumKong 位于 `numkong/` 子模块。度量 / SIMD 相关修复请优先向上游贡献，再在本仓库更新子模块指针。

## 文档

使用 Sphinx + Doxygen 为 C++ 头文件生成文档：

```sh
pip install -r docs/requirements.txt
cd docs && doxygen conf.dox && make html
```
