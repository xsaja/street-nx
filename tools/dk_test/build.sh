#!/usr/bin/env bash
# Builds build-dktest/dk_test.nro: run in the MSYS2 bash of D:\msys64 (devkitPro in /opt/devkitpro).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
cmake -S "$HERE" -B "$ROOT/build-dktest" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DDK_DEBUG=${DK_DEBUG:-ON}
ninja -C "$ROOT/build-dktest" -j"${JOBS:-8}" dk_test_nro
