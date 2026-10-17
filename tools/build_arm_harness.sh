#!/usr/bin/env bash
# ARM Linux harness (with recompiler) → build-arm/harness. Run: qemu-arm build-arm/harness --dynarec ...
# Needs: gcc-arm-linux-gnueabihf, qemu-arm, ~/armzlib/inst (zlib 1.3.1 static ARM build)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
cmake -S tools/harness -B build-arm "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/harness/arm-linux.cmake" -DRSTV_DYNAREC=ON >/dev/null
cmake --build build-arm -j8
