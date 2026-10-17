#!/usr/bin/env bash
# Build and run the PC harness with gprof and print the top functions by time.
#   tools/profile_pc.sh GAME FRAMES [extra harness args...]
set -euo pipefail
cd "$(dirname "$0")/.."
GAME="${1:-sandor}"; FRAMES="${2:-2400}"; shift $(( $# >= 2 ? 2 : $# ))
cmake -S tools/harness -B build-pc-prof -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_FLAGS="-pg -fno-omit-frame-pointer -fno-inline-functions" -DCMAKE_CXX_FLAGS="-pg -fno-inline-functions" \
	-DCMAKE_EXE_LINKER_FLAGS="-pg" >/dev/null
cmake --build build-pc-prof -j >/dev/null
mkdir -p artifacts/prof && cd artifacts/prof
../../build-pc-prof/harness --roms ../../roms --game "$GAME" --frames "$FRAMES" --checkpoint "$FRAMES" "$@" >/dev/null
gprof -b -p ../../build-pc-prof/harness gmon.out | head -45
