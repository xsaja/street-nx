#!/usr/bin/env bash
# Builds build-nvktest/nvk_test.nro: run in the MSYS2 bash of D:\msys64 (devkitPro in /opt/devkitpro).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
cmake -S "$HERE" -B "$ROOT/build-nvktest" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release
ninja -C "$ROOT/build-nvktest" -j"${JOBS:-8}" nvk_test_nro
