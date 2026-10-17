#!/usr/bin/env bash
# Compare video/audio hashes of 5 sets x 3600 frames against tools/baseline.txt (sync SCSP harness, deterministic).
#   tools/hashcheck.sh          compare
#   tools/hashcheck.sh --update update baseline (intended changes only, record reason in PERF.md)
#   HARNESS_ARGS="--vdp1thread" tools/hashcheck.sh  compare against the same baseline with extra harness options
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=$(mktemp)
for g in sandor thunt thuntk sasissu sanjeon; do
	for f in 1200 2400 3600; do :; done
	./build-pc/harness --roms roms --game "$g" --frames 3600 --checkpoint 1200 ${HARNESS_ARGS:-} 2>/dev/null |
		awk -v g="$g" '/^frame/ {print g, $2, $7, $9}' >> "$OUT"
done
if [ "${1:-}" = "--update" ]; then
	cp "$OUT" tools/baseline.txt
	echo "baseline updated"; cat tools/baseline.txt
elif diff -u tools/baseline.txt "$OUT"; then
	echo "HASH OK (5 sets)"
else
	echo "HASH MISMATCH"; exit 1
fi
rm -f "$OUT"
