#!/usr/bin/env bash
# ADR-243 / 247: copy a device's shader cache (ux0:data/restv/shaders: compiled shaders *.gxp, and the variant sources and
# per-game lists cv_ / nv_ / rv_ / cl_*.txt the loading screen preloads from) into vita/shader_pack, which the VPK ships
# as app0:shaders/. Run before a release, after playing every parent game on the device with the release build (a new
# version empties the device cache, ADR-246, so it then holds only that version's files).
#   tools/pull_shaders.sh          adds what the device has and the pack lacks
#   tools/pull_shaders.sh --prune  also removes packed files the device does not have (only on a cache refilled as above:
#                                  a freshly emptied cache would remove the whole pack)
# A cl_ list is always taken again (it grows on the device). VITA_IP as for deploy.
set -e
cd "$(dirname "$0")/.."
IP="${VITA_IP:-192.168.0.33}"
DST=vita/shader_pack
mkdir -p "$DST"
list=$(curl -s --max-time 20 --list-only "ftp://$IP:1337/ux0:/data/restv/shaders/" | tr -d '\r' | grep -E '\.gxp$|^(cv|nv|rv|cl)_.*\.txt$' || true)
[ -n "$list" ] || { echo "no shaders listed (device unreachable or cache empty)"; exit 1; }
n=0
for f in $list; do
	case "$f" in cl_*) rm -f "$DST/$f" ;; esac
	[ -s "$DST/$f" ] || { curl -s --max-time 20 -o "$DST/$f" "ftp://$IP:1337/ux0:/data/restv/shaders/$f"; n=$((n + 1)); }
done
if [ "$1" = "--prune" ]; then
	for f in "$DST"/*; do
		b=$(basename "$f")
		echo "$list" | grep -qx "$b" || { rm -f "$f"; echo "removed $b"; }
	done
fi
echo "$(ls "$DST" | wc -l) files in $DST ($n taken), $(du -sh "$DST" | cut -f1)"
