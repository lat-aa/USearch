#!/bin/bash -eu
# ClusterFuzzLite build entry (runs inside the CIFUZZ docker image).
cd "$SRC"
# When checked out as the repo root:
ROOT="${GITHUB_WORKSPACE:-$SRC}"
if [[ -f "$ROOT/CMakeLists.txt" ]]; then
  cd "$ROOT"
elif [[ -f "$SRC/usearch/CMakeLists.txt" ]]; then
  cd "$SRC/usearch"
fi
git submodule update --init --recursive || true
cmake -B build_fuzz \
  -D CMAKE_BUILD_TYPE=RelWithDebInfo \
  -D USEARCH_BUILD_TEST_CPP=1 \
  -D USEARCH_BUILD_FUZZ=ON \
  -D USEARCH_BUILD_BENCH_CPP=0 \
  -D USEARCH_USE_NUMKONG=0 \
  -D USEARCH_SANITIZE_DEBUG=OFF
cmake --build build_fuzz --target fuzz_index -j"$(nproc)"
cp build_fuzz/fuzz_index "$OUT/"
mkdir -p "$OUT/fuzz_index_seed_corpus"
cp -r fuzz/corpus/* "$OUT/fuzz_index_seed_corpus/" 2>/dev/null || true
