#!/bin/bash
# usage: hashrun.sh <tag>  -> /tmp/hb/<tag>_<game>.txt (full frame lines without fps)
cd "/mnt/e/Dev/Vita Port/ReST-V"
for g in thuntk sandor sasissu; do
  timeout 35 ./build-pc/harness --roms roms --game $g --frames 4200 --checkpoint 300 --coin 2400 --start 2550 2>/dev/null | grep "^frame" | sed -E 's/ +[0-9.]+ fps//' > /tmp/hb/$1_$g.txt
done
wc -l /tmp/hb/$1_*.txt | tail -1
