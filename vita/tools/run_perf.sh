#!/usr/bin/env bash
# For Git Bash (Windows): deploy eboot → run → wait until N new perf log lines arrive and print them. Also reports new crash dumps.
#   vita/tools/run_perf.sh [game=thuntk] [lines=3] [timeout_s=240]
#   STATE="frame [name]" loads ux0:data/restv/state/<name or game>.yss at that frame (after BIOS boot) to measure the same scene (no auto input).
#   Without STATE, auto input (2400 coin, 2550 start). Measurement files are deleted at the end (so normal play is unaffected).
#   UDPLOG=<file written by udp_log.py> tracks progress through that UDP log (perf line every 60 frames); FTP is used only at start and end.
#   (polling FTP every 4 s opened hundreds of connections per run, stalled vitacompanion FTP and slowed the Vita)
#   If the UDP log stops for over 25 s, finish with STALL (app did not start or hung).
cd "$(dirname "$0")/../.."
GAME="${1:-thuntk}"; LINES="${2:-3}"; TMO="${3:-240}"
IP="${VITA_IP:-192.168.0.33}"
FTP="ftp://$IP:1337/ux0:/data"
before=$(curl -s --max-time 8 --list-only "$FTP/" | grep 'eboot.bin.psp2dmp' | sort | tail -1)
# one-shot autostart request (the last pick in game.txt is left untouched)
printf "%s" "$GAME" | curl -s --max-time 10 -T - "$FTP/restv/autostart"
if [ -n "$STATE" ]; then
	printf '%s' "$STATE" | curl -s --max-time 10 -T - "$FTP/restv/loadstate"
	[ -n "$AUTO" ] && printf '%s\n' "$AUTO" | tr ';' '\n' | curl -s --max-time 10 -T - "$FTP/restv/autoinput.txt"
else
	# MASH=frame: from that frame random lever/button mashing + repeated coin/start (sanjeon only gives hockey; do not use for long tests)
	# AUTO="frame input;frame input;..." : extra auto input (coin start up down left right b1..b4, 6 frames each)
	{ printf '2400 coin\n2550 start\n'; [ -n "$MASH" ] && printf '%s mash\n' "$MASH"; [ -n "$AUTO" ] && printf '%s\n' "$AUTO" | tr ';' '\n'; } | curl -s --max-time 10 -T - "$FTP/restv/autoinput.txt"
fi
curl -s --max-time 10 -Q "DELE ux0:/data/restv/perf_$GAME.txt" "$FTP/restv/" >/dev/null 2>&1
n0=0
[ -n "$UDPLOG" ] && [ -f "$UDPLOG" ] && n0=$(wc -l < "$UDPLOG")
powershell.exe -ExecutionPolicy Bypass -File "vita/tools/deploy.ps1" -Eboot "build-vita/eboot.bin" -VitaIp "$IP" >/dev/null || { echo "deploy failed"; exit 1; }
end=$(( $(date +%s) + TMO ))
target=$(( LINES * 300 ))
if [ -n "$UDPLOG" ] && [ -f "$UDPLOG" ]; then
	last_n=$n0; last_t=$(date +%s); grace=60 # wait up to 60 s for the first log (boot, shader setup)
	while [ "$(date +%s)" -lt "$end" ]; do
		sleep 3
		n=$(wc -l < "$UDPLOG")
		if [ "$n" != "$last_n" ]; then last_n=$n; last_t=$(date +%s); grace=25; fi
		# only look at lines after the new run's start ("game:") so lines from the previous run still exiting are not taken as results
		f=$(tail -n +$((n0 + 1)) "$UDPLOG" | sed -n '/\] game: /,$p' | grep -a -o '\] f[0-9]* emu' | tail -1 | tr -dc '0-9')
		[ -n "$f" ] && [ "$f" -ge "$target" ] && break
		[ $(( $(date +%s) - last_t )) -gt $grace ] && { echo "STALL: no UDP log for over ${grace}s (last f${f:-none})"; break; }
	done
else
	while [ "$(date +%s)" -lt "$end" ]; do
		sleep 15
		c=$(curl -s --max-time 5 "$FTP/restv/perf_$GAME.txt")
		[ "$(printf '%s\n' "$c" | grep -c '^f')" -ge "$LINES" ] && break
	done
fi
out=$(curl -s --max-time 8 "$FTP/restv/perf_$GAME.txt")
if [ -z "$out" ] && [ -n "$UDPLOG" ] && [ -f "$UDPLOG" ]; then
	# FTP failed: use the same perf line from the UDP log (multiple of 300 frames) instead
	out=$(tail -n +$((n0 + 1)) "$UDPLOG" | sed -n '/\] game: /,$p' | grep -a -o 'f[0-9]* emu .*' | awk '{f=substr($1,2)+0; if (f % 300 == 0) print}')
fi
echo "$out"
now=$(curl -s --max-time 8 --list-only "$FTP/" | grep 'eboot.bin.psp2dmp' | sort | tail -1)
[ -n "$now" ] && [ "$now" != "$before" ] && echo "CRASH: $now"
for f in loadstate autoinput.txt; do curl -s --max-time 8 -Q "DELE ux0:/data/restv/$f" "$FTP/restv/" >/dev/null 2>&1; done
err=$(curl -s --max-time 5 "$FTP/restv/last_error.txt"); [ -n "$err" ] && echo "last_error: $err"
exit 0
