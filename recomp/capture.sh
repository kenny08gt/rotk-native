#!/bin/sh
# Run the native build for N seconds, capturing only its window every few seconds.
# usage: ./capture.sh <outdir> [seconds=40] [interval=8]
set -u
cd "$(dirname "$0")"
out=$1; secs=${2:-40}; step=${3:-8}
mkdir -p "$out"
ISO=${ROTK_ISO:-$(cat "$HOME/Library/Application Support/rotk/disc_path" 2>/dev/null)}
./build/rotk ../extracted/sys "$ISO" ../decomp/symbols_final.tsv "${MAXD:-40000000000}" "$secs" > "$out/run.log" 2> "$out/run.err" &
pid=$!
t=0
while kill -0 $pid 2>/dev/null; do
  sleep "$step"; t=$((t + step))
  id=$(tools/winid rotk)
  [ -n "$id" ] && screencapture -x -o -l"$id" "$out/frame_$t.png"
done
wait $pid; echo "exit=$?"
