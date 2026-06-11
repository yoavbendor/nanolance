#!/usr/bin/env bash
# Phase-B scaling benchmark: time the converter (L1 parse + optional L2/L3/L4 decode) across CPU thread
# counts and against the sequential reference path, on one capture. Use it to find the sweet spot on a
# many-core host (e.g. 192 cores — sweep --threads 4,8,16,32,64,128,164).
#
# IMPORTANT: end-to-end the file read + the single-threaded Lance write usually DOMINATE, so thread count
# barely moves the total. To actually see Phase-B (the parallel part) scale, pass --no-write: it runs
# scan+parse+decode but skips all Lance output, isolating the CPU work. Pass --decode-l2l3 to give Phase B
# more weight (it adds the L2/L3/L4 decode passes).
#
# Run with a window >= the file size (the default 4 GiB; raise it for bigger files) so the whole capture
# is ONE bulk dispatch and the thread count is the only variable.
#
# usage:
#   bench/decode_bench.sh --input cap.pcapng [--threads 4,8,16,32,64,128,164] [--decode-l2l3] [--no-write] \
#       [--window-bytes N] [--repeat 3] [--exe build/examples/pcapng2lance/pcapng2lance]
set -uo pipefail

INPUT=""
THREADS="1,2,4,8,16,32,64,128"
EXTRA=""                   # extra converter flags (--decode-l2l3, --no-write)
WINDOW=4294967296          # 4 GiB: one window for typical captures (raise for bigger files)
REPEAT=3
EXE="build/examples/pcapng2lance/pcapng2lance"

while [ $# -gt 0 ]; do
  case "$1" in
    --input) INPUT="$2"; shift 2;;
    --threads) THREADS="$2"; shift 2;;
    --decode-l2l3) EXTRA="$EXTRA --decode-l2l3"; shift;;
    --no-write) EXTRA="$EXTRA --no-write"; shift;;
    --window-bytes) WINDOW="$2"; shift 2;;
    --repeat) REPEAT="$2"; shift 2;;
    --exe) EXE="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[ -n "$INPUT" ] || { echo "error: --input required" >&2; exit 2; }
[ -x "$EXE" ] || command -v "$EXE" >/dev/null || { echo "error: exe not found: $EXE" >&2; exit 2; }
[ -f "$INPUT" ] || { echo "error: input not found: $INPUT" >&2; exit 2; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
ERRLOG="$TMP/err.log"

# Preflight: one visible run so a misconfig/stale binary fails loudly here, not silently as "2 ms".
echo ">> preflight: $EXE $INPUT <out> $EXTRA --window-bytes $WINDOW"
if ! "$EXE" "$INPUT" "$TMP/o.lance" $EXTRA --window-bytes "$WINDOW" >/dev/null 2>"$ERRLOG"; then
  echo "ERROR: converter failed on preflight — not benchmarking. stderr:" >&2
  cat "$ERRLOG" >&2
  exit 1
fi
rm -rf "$TMP/o.lance"

fsize=$(stat -c%s "$INPUT" 2>/dev/null || echo "?")
echo "input: $INPUT (${fsize} B)   window=${WINDOW}   flags='${EXTRA:- (L1 only, with write)}'   best of ${REPEAT}"
echo "----------------------------------------"
printf "%-14s | %10s\n" "mode" "best ms"
printf "%-14s-+-%10s\n" "--------------" "----------"

# Run EXE (positionals first, flags after) REPEAT times; echo the best (min) wall-clock ms. Abort on any
# non-zero exit so a failure is never silently timed.
bench_run() {
  local best=2147483647
  for _ in $(seq 1 "$REPEAT"); do
    rm -rf "$TMP/o.lance"
    local s; s=$(date +%s%3N)
    if ! "$EXE" "$INPUT" "$TMP/o.lance" "$@" $EXTRA --window-bytes "$WINDOW" >/dev/null 2>"$ERRLOG"; then
      echo "RUN FAILED ($*):" >&2; cat "$ERRLOG" >&2; exit 1
    fi
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
echo "(best of ${REPEAT} wall-clock runs each; lower = faster. Use --no-write to isolate Phase B.)"
