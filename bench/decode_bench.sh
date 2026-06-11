#!/usr/bin/env bash
# Phase-B scaling benchmark: time the converter (L1 parse + optional L2/L3/L4 decode) across CPU thread
# counts and against the sequential reference path, on one capture. Use it to find the sweet spot on a
# many-core host (e.g. 192 cores — sweep --threads 4,8,16,32,64,128,164 and watch where it stops helping).
#
# End-to-end wall time per config. Run with a window >= the file size (the default) so the whole capture
# is ONE bulk dispatch and the thread count is the only variable. Note: file read + the single-threaded
# Lance write are memory/IO-bound and often dominate, so differences can be small for L1 alone — pass
# --decode-l2l3 to give Phase B (the parallel part) more weight.
#
# usage:
#   bench/decode_bench.sh --input cap.pcapng [--threads 4,8,16,32,64,128,164] [--decode-l2l3] \
#       [--window-bytes N] [--repeat 3] [--exe build/examples/pcapng2lance/pcapng2lance]
set -uo pipefail

INPUT=""
THREADS="1,2,4,8,16,32,64,128"
DECODE=""
WINDOW=4294967296          # 4 GiB: one window for typical captures (override for >4 GiB files)
REPEAT=3
EXE="build/examples/pcapng2lance/pcapng2lance"

while [ $# -gt 0 ]; do
  case "$1" in
    --input) INPUT="$2"; shift 2;;
    --threads) THREADS="$2"; shift 2;;
    --decode-l2l3) DECODE="--decode-l2l3"; shift;;
    --window-bytes) WINDOW="$2"; shift 2;;
    --repeat) REPEAT="$2"; shift 2;;
    --exe) EXE="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[ -n "$INPUT" ] || { echo "error: --input required" >&2; exit 2; }
[ -x "$EXE" ] || command -v "$EXE" >/dev/null || { echo "error: exe not found: $EXE" >&2; exit 2; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fsize=$(stat -c%s "$INPUT" 2>/dev/null || echo "?")
echo "input: $INPUT (${fsize} B)   window=${WINDOW}   phase-B work='${DECODE:-L1 only}'   best of ${REPEAT}"
echo "----------------------------------------"
printf "%-14s | %10s\n" "mode" "best ms"
printf "%-14s-+-%10s\n" "--------------" "----------"

# Run EXE with the given extra args REPEAT times; echo the best (min) wall-clock ms.
bench_run() {
  local best=2147483647
  for _ in $(seq 1 "$REPEAT"); do
    rm -rf "$TMP/o.lance"
    local s; s=$(date +%s%3N)
    "$EXE" $DECODE "$@" --window-bytes "$WINDOW" "$INPUT" "$TMP/o.lance" >/dev/null 2>&1
    local e; e=$(date +%s%3N)
    local ms=$((e - s))
    [ "$ms" -lt "$best" ] && best=$ms
  done
  echo "$best"
}

printf "%-14s | %10s\n" "sequential" "$(bench_run --sequential)"
IFS=',' read -r -a TS <<< "$THREADS"
for t in "${TS[@]}"; do
  printf "%-14s | %10s\n" "bulk x${t}" "$(bench_run --threads "$t")"
done
echo "----------------------------------------"
echo "(best of ${REPEAT} end-to-end wall-clock runs each; lower = faster)"
