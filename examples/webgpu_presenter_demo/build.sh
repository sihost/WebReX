#!/usr/bin/env sh
# Builds the WebGPU presenter demo into out/web next to this file.
#   ./build.sh [wasm64 SDK install prefix, default <repo>/out/install/wasm64]
# Needs emsdk on PATH (source emsdk_env.sh first) and the SDK built + installed with the
# wasm64-release preset (see WEB_PORT.md).
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${1:-$SRC/../../out/install/wasm64}"
: "${EMSDK:?EMSDK is not set - source emsdk_env.sh first}"
FLAGS="-m64 -pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm"
cmake -S "$SRC" -B "$SRC/out" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" \
  -Drexglue_DIR="$PREFIX/lib/cmake/rexglue" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
  -DCMAKE_EXE_LINKER_FLAGS="-m64 -pthread -fwasm-exceptions -sSUPPORT_LONGJMP=wasm"
cmake --build "$SRC/out"
echo "built $SRC/out/web - run: node $SRC/serve.mjs"
