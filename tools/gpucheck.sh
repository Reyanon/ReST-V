#!/usr/bin/env bash
# Compare C port of the Vita NBG shader against vidsoft layers (PC). Coin and start to cover gameplay
cd "$(dirname "$0")/.."
for g in ${@:-sandor thunt thuntk sasissu sanjeon}; do
	echo "== $g"
	./build-pc/harness --roms roms --game "$g" --frames 3600 --checkpoint 0 --coin 2400 --start 2550 --gpucheck 2>/dev/null | grep -E "gpucheck|quadcheck|rbgcheck" | head -30
done
