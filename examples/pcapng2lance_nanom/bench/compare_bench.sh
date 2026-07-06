#!/usr/bin/env bash
# Head-to-head benchmark: the nanom L1 converter (pcapng2lance_nanom) vs the nanotins one (pcapng2lance)
# on the same capture, producing the same packets.lance. Reports wall time + packets/s for each, best of
# --repeat runs. Pass --no-write to isolate scan+parse (nanom Phase A/B) from the shared Lance write/IO,
# which otherwise dominates. With both datasets written, it also confirms they are byte-identical.
#
# Build first (from the nanolance root):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
#         -DNANOLANCE_BUILD_EXAMPLES=ON -DNANOLANCE_BUILD_PCAPNG2LANCE_NANOM=ON
#   cmake --build build --target pcapng2lance pcapng2lance_nanom -j
#
# usage:
#   examples/pcapng2lance_nanom/bench/compare_bench.sh --input cap.pcapng [--repeat 5] [--no-write] \
#       [--nanom build/examples/pcapng2lance_nanom/pcapng2lance_nanom] \
#       [--nanotins build/examples/pcapng2lance/pcapng2lance]
set -uo pipefail

INPUT=""
REPEAT=5
EXTRA=""
NM="build/examples/pcapng2lance_nanom/pcapng2lance_nanom"
NT="build/examples/pcapng2lance/pcapng2lance"

while [ $# -gt 0 ]; do
  case "$1" in
    --input) INPUT="$2"; shift 2;;
    --repeat) REPEAT="$2"; shift 2;;
    --no-write) EXTRA="$EXTRA --no-write"; shift;;
    --nanom) NM="$2"; shift 2;;
    --nanotins) NT="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[ -n "$INPUT" ] || { echo "error: --input required" >&2; exit 2; }
[ -f "$INPUT" ] || { echo "error: input not found: $INPUT" >&2; exit 2; }
[ -x "$NM" ] || { echo "error: nanom exe not found: $NM (build pcapng2lance_nanom)" >&2; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Extract packet count from the converter's own summary line ("<exe>: N packets, ...").
pkt_count() { sed -n 's/.*: \([0-9][0-9]*\) packets.*/\1/p' <<<"$1" | head -1; }

bench_one() {
  local exe="$1" label="$2" out="$3"
  [ -x "$exe" ] || command -v "$exe" >/dev/null || { echo "  $label: (exe missing, skipped)"; return; }
  local best="" pkts="" t
  for _ in $(seq 1 "$REPEAT"); do
    rm -rf "$out"
    local start end summary
    start=$(date +%s.%N)
    summary=$("$exe" $EXTRA "$INPUT" "$out" 2>&1) || { echo "  $label: FAILED: $summary"; return 1; }
    end=$(date +%s.%N)
    t=$(awk -v a="$start" -v b="$end" 'BEGIN{printf "%.4f", b-a}')
    [ -z "$best" ] && best="$t"
    awk -v a="$t" -v b="$best" 'BEGIN{exit !(a<b)}' && best="$t"
    pkts=$(pkt_count "$summary")
  done
  local pps
  pps=$(awk -v p="$pkts" -v s="$best" 'BEGIN{ if (s>0) printf "%.0f", p/s; else print "inf" }')
  printf "  %-10s best=%ss  packets=%s  %s pkt/s\n" "$label" "$best" "$pkts" "$pps"
}

echo "== pcapng2lance: nanom vs nanotins =="
echo "input: $INPUT   repeat: $REPEAT   flags:${EXTRA:- (write)}"
bench_one "$NM" "nanom" "$TMP/nm.lance"
bench_one "$NT" "nanotins" "$TMP/nt.lance"

# Byte-for-byte equivalence check (only meaningful when both wrote a dataset).
if [ -z "$EXTRA" ] && [ -d "$TMP/nm.lance" ] && [ -d "$TMP/nt.lance" ]; then
  if command -v python3 >/dev/null && python3 -c "import lance" 2>/dev/null; then
    python3 - "$TMP/nm.lance" "$TMP/nt.lance" <<'PY'
import sys, lance
nm = lance.dataset(sys.argv[1]).to_table().to_pydict()
nt = lance.dataset(sys.argv[2]).to_table().to_pydict()
ok = list(nm.keys()) == list(nt.keys()) and all(nm[k] == nt[k] for k in nt)
print(f"  equivalence: {'IDENTICAL' if ok else 'DIFFERS'} ({len(nm['packet_id'])} rows)")
sys.exit(0 if ok else 1)
PY
  else
    echo "  equivalence: (pip install pylance to verify byte-identical output)"
  fi
fi
