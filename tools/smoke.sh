#!/usr/bin/env bash
# Run the 5 primary target sets in the PC harness and save checkpoint hashes and screenshots.
#   tools/smoke.sh [frames=3600] [shots=2100,2700,3300,3600] [extra harness args...]
set -euo pipefail
cd "$(dirname "$0")/.."
FRAMES="${1:-3600}"
SHOTS="${2:-2100,2700,3300,3600}"
shift $(( $# >= 2 ? 2 : $# ))
mkdir -p artifacts/shots
for g in sandor thunt thuntk sasissu sanjeon; do
	echo "== $g"
	./build-pc/harness --roms roms --game "$g" --frames "$FRAMES" --checkpoint "$FRAMES" \
		--shot "$SHOTS" --out artifacts/shots "$@" 2>&1 | grep -v '^init'
done
