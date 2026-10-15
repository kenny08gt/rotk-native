#!/bin/sh
# Play from boot into the first big Helm's Deep fight with scripted presses and report the
# average speed and frame rate during the fight.
# usage: ./bench.sh <label> [seconds=330]   (settings come from the environment; ROTK_BIN picks the binary)
# Prints: <label> fight cpu <c>% speed <x>% fps <y> (window 250 s to the end); the full PERF_LOG is in runs/bench/<label>.log.
set -u
cd "$(dirname "$0")"
label=$1; secs=${2:-330}
ISO=${ROTK_ISO:-$(cat "$HOME/Library/Application Support/rotk/disc_path" 2>/dev/null)}
P=""
for t in $(seq 15 1 "$secs"); do P="$P,A@$t"; done        # menus, new game, then attacks
for t in $(seq 300.5 2 "$secs"); do P="$P,X@$t"; done
mkdir -p runs/bench
log=runs/bench/$label.log
echo "load before: $(uptime)" > "$log"
# /usr/bin/time -l adds "instructions retired" to the .err file: host instructions per guest cycle
# is the comparison that holds up when other programs load the Mac.
AUTOPRESS="${P#,}" PERF_LOG=1 MEMCARD=none /usr/bin/time -l "${ROTK_BIN:-./build/rotk}" ../extracted/sys "$ISO" \
  ../decomp/symbols_final.tsv 40000000000 "$secs" >> "$log" 2> "runs/bench/$label.err"
echo "load after: $(uptime)" >> "$log"
insns=$(awk '/instructions retired/ {print $1}' "runs/bench/$label.err")
gcyc=$(awk '/^guest cycles:/ {print $3}' "$log")
[ -n "$insns" ] && [ -n "$gcyc" ] && awk -v i="$insns" -v g="$gcyc" -v l="$label" \
  'BEGIN { printf "%s host instructions per guest cycle: %.2f\n", l, i / g }'
# Fight averages. "cpu" is guest time per second of the game thread's CPU time; it and the
# wall-clock speed and fps all shift when other programs load the Mac (see the uptime lines).
awk -v label="$label" '/^perf/ { t = $2 + 0; if (t >= 250) { cap += $4 + 0; c += $9 + 0; fps += $(NF - 1); n++ } }
  END { if (n) printf "%s fight cpu %.0f%% speed %.0f%% fps %.1f (%d samples)\n", label, c / n, cap / n, fps / n, n }' "$log"
