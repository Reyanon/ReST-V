#!/usr/bin/env bash
# Top VDP2 register combinations per game (by frame count), feature survey for the new renderer
cd "$(dirname "$0")/.."
for g in "${@:-sandor thunt thuntk sasissu sanjeon}"; do
	for gg in $g; do
		echo "== $gg"
		./build-pc/harness --roms roms --game "$gg" --frames 6000 --checkpoint 100000 --coin 2400 --start 2550 --vdp2stats 2>/dev/null | grep '^ ' | sort -rn | head -8
	done
done
