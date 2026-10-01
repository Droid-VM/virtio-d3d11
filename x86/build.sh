#!/usr/bin/env bash
# Build the 32-bit D3D10/11 UMD and WGL ICD alongside the ARM64/EC outputs.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
win3d="$(cd "$here/../.." && pwd)"
export PREFIX=i686-w64-mingw32
export JOBS="${JOBS:-8}"
BUILD_DIR=build-x86 bash "$win3d/build_umd.sh" LTO=
ARCH=x86 BUILD_DIR="$win3d/../mesa/build-wgl-x86" \
    DIST_DIR="$win3d/dist/opengl-x86" bash "$win3d/build_opengl.sh"
