#!/usr/bin/env bash
# Builds the Switch NRO: run in the MSYS2 bash of D:\msys64 (devkitPro's packages in /opt/devkitpro).
# TARGET=street_host_check compiles only the host and runtime; JOBS lowers the parallel jobs if GCC runs out of memory.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
BUILD="${BUILD_DIR:-$ROOT/build-switch}"
cmake -S "$HERE" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DSTREET_VERBOSE_LOG=${VERBOSE:-OFF}
ninja -C "$BUILD" -j"${JOBS:-6}" ${TARGET:-street_nro}
