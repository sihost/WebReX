#!/usr/bin/env sh
# Sparse checkout of Dawn at the emdawnwebgpu revision (v20260423.175430 = 31e25af2) into
# thirdparty/dawn, with only what Tint's SPIR-V reader + WGSL writer need (~150 MB).
# Usage: scripts/fetch_dawn.sh [sdk_root]   (default: this repository)
set -e
SDK="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
REV=31e25af254ab572c77054edec4946d2244e184dd
DST="$SDK/thirdparty/dawn"
[ -d "$DST/.git" ] || git clone --filter=blob:none --no-checkout https://github.com/google/dawn.git "$DST"
cd "$DST"
git sparse-checkout init --cone
git sparse-checkout set src/cmake src/tint src/utils include third_party
git checkout --quiet "$REV"
git submodule update --init --depth 1 third_party/spirv-headers/src third_party/spirv-tools/src third_party/abseil-cpp
git log --oneline -1
