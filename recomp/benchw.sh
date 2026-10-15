#!/bin/sh
# Deterministic fight benchmark: headless and unpaced, button presses at fixed guest times, so
# every run executes exactly the same guest work. Measures the first big Helm's Deep fight
# (guest seconds 250-330) and prints the game thread's CPU time, host instructions, and a hash
# of guest RAM at the end (equal hashes = the same game state, so builds can be compared exactly).
# usage: ./benchw.sh <label> [runs=1]   (ROTK_BIN picks the binary)
set -u
cd "$(dirname "$0")"
label=$1; runs=${2:-1}
ISO=${ROTK_ISO:-$(cat "$HOME/Library/Application Support/rotk/disc_path" 2>/dev/null)}
P=""
for t in $(seq 15 1 330); do P="$P,A@$t"; done
for t in $(seq 300.5 2 330); do P="$P,X@$t"; done
mkdir -p runs/benchw
for i in $(seq 1 "$runs"); do
  AUTOPRESS="${P#,}" BENCH_WINDOW=250,330 NO_GPU=1 NO_THROTTLE=1 NO_AUDIO=1 MEMCARD=none \
    "${ROTK_BIN:-./build/rotk}" ../extracted/sys "$ISO" ../decomp/symbols_final.tsv 40000000000 1800 \
    > "runs/benchw/$label.$i.log" 2> "runs/benchw/$label.$i.err"
  echo "$label#$i $(grep '^bench window' "runs/benchw/$label.$i.log") | load $(uptime | sed 's/.*averages: //')"
done
