#!/usr/bin/env bash
# 离线依赖重建 Linux api → .local/api（不访问 GitHub）
set -euo pipefail
ROOT=/mnt/e/data/USearch
B="${HOME}/usearch-build"
mkdir -p "$B/_deps"
for d in json-src httplib-src sqlite_amalgamation-src tomlplusplus-src clipp-src; do
  if [ ! -f "$B/_deps/$d/CMakeLists.txt" ] && [ -d "$ROOT/build/_deps/$d" ]; then
    echo "copy $d"
    rm -rf "$B/_deps/$d"
    cp -a "$ROOT/build/_deps/$d" "$B/_deps/"
  fi
done
# llama：优先已有；否则从 tar
if [ ! -f "$B/_deps/llama-src/CMakeLists.txt" ]; then
  if [ -f "$ROOT/.local/llama-src.tar" ]; then
    tar xf "$ROOT/.local/llama-src.tar" -C "$B/_deps"
  elif [ -d "$ROOT/build/_deps/llama-src" ]; then
    cp -a "$ROOT/build/_deps/llama-src" "$B/_deps/"
  fi
fi
ls "$B/_deps"

cd "$ROOT"
# 清掉半残 cache，保留已编对象若可
rm -f "$B/CMakeCache.txt"
cmake -B "$B" \
  -DUSEARCH_BUILD_API=ON \
  -DUSEARCH_BUILD_TEST_CPP=OFF \
  -DUSEARCH_BUILD_BENCH_CPP=OFF \
  -DUSEARCH_USE_NUMKONG=OFF \
  -DFETCHCONTENT_SOURCE_DIR_JSON="$B/_deps/json-src" \
  -DFETCHCONTENT_SOURCE_DIR_HTTPLIB="$B/_deps/httplib-src" \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  ${LLAMA_SRC:+-DLLAMA_SRC=$LLAMA_SRC}

cmake --build "$B" --target api -j"$(nproc)"
cp -a "$B/api" "$ROOT/.local/api"
# 同步到 smoke 工作副本
mkdir -p "$HOME/usearch-smoke/.local"
cp -a "$B/api" "$HOME/usearch-smoke/.local/api"
echo REBUILD_OK
ls -la "$ROOT/.local/api"
