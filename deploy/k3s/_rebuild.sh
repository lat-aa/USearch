#!/bin/sh
# Incremental Ubuntu glibc rebuild of api → .local/api (Linux FS only; no /mnt cmake).
set -e
if [ -f /mnt/e/data/USearch/CMakeLists.txt ]; then
  ROOT=/mnt/e/data/USearch
else
  ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
fi

# 9p /mnt is slow; keep objects on ext4.
BUILD="${USEARCH_BUILD:-}"
if [ -z "$BUILD" ]; then
  if [ "$(id -u)" = 0 ]; then
    BUILD=/root/usearch-build
  else
    BUILD="$HOME/usearch-build"
  fi
fi
case "$BUILD" in
  /mnt/*)
    echo "error: USEARCH_BUILD must not be under /mnt (9p). use /root/usearch-build"
    exit 1
    ;;
esac

JOBS="${JOBS:-$(nproc)}"
LLAMA_SRC="${LLAMA_SRC:-$BUILD/_deps/llama-src}"
TAR="$ROOT/.local/llama-src.tar"

if [ ! -f "$LLAMA_SRC/CMakeLists.txt" ] && [ -f "$TAR" ]; then
  mkdir -p "$BUILD/_deps"
  tar xf "$TAR" -C "$BUILD/_deps"
fi
export LLAMA_SRC

GEN=""
if [ ! -f "$BUILD/CMakeCache.txt" ] && command -v ninja >/dev/null 2>&1; then
  GEN="-G Ninja"
fi
LAUNCHER=""
if command -v ccache >/dev/null 2>&1; then
  LAUNCHER="-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache -DGGML_CCACHE=ON"
fi

need_cfg=0
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  need_cfg=1
fi

if [ "$need_cfg" = 1 ]; then
  cmake -S "$ROOT" -B "$BUILD" $GEN \
    -DUSEARCH_BUILD_API=ON -DUSEARCH_BUILD_TEST_CPP=OFF -DUSEARCH_BUILD_BENCH_CPP=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DFETCHCONTENT_SOURCE_DIR_LLAMA="$LLAMA_SRC" \
    -DFETCHCONTENT_UPDATES_DISCONNECTED=ON \
    $LAUNCHER
fi

cmake --build "$BUILD" --config Release --target api -j"$JOBS"

mkdir -p "$ROOT/.local/lib"
cp -f "$BUILD/api" "$ROOT/.local/api"
chmod +x "$ROOT/.local/api"
for lib in libstdc++.so.6 libgomp.so.1 libgcc_s.so.1 libssl.so.3 libcrypto.so.3; do
  src=$(ldconfig -p 2>/dev/null | awk -v n="$lib" '$1 == n { print $NF; exit }')
  if [ -n "$src" ] && [ -f "$src" ]; then
    cp -fL "$src" "$ROOT/.local/lib/$lib"
  fi
done
echo BUILD_OK
