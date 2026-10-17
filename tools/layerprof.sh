#!/usr/bin/env bash
# Per-game layer time (PC, average over last 600 frames). Coin and start to cover gameplay
cd "$(dirname "$0")/.."
for g in ${@:-sandor thunt sasissu}; do
	echo "== $g"
	./build-pc/harness --roms roms --game "$g" --frames 4800 --checkpoint 1200 --coin 2400 --start 2550 2>/dev/null | grep -E "prof|detail|layers" | tail -6
done
