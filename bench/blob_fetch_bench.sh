#!/usr/bin/env bash
# Blob-fetch benchmark: compare nanolance (nano_lance_fetch_external_blob, the AWS-C++-SDK path) against
# pylance (take_blobs(), the Rust core) on the SAME lance.blob.v2 dataset of external references.
#
# Flow:
#   1. read each object's size (stat for file://, `aws s3api head-object` for s3://),
#   2. nlance_blobgen tiles every object into consecutive random packet-sized chunks -> one dataset,
#   3. each fetcher emits `index <TAB> md5 <TAB> fetch_nanos` per blob,
#   4. assert md5 agreement per index across BOTH implementations (correctness),
#   5. time with a warm/cold-aware schedule: pylance x3 cold, then rounds that rotate which impl goes
#      first (cancels ordering/warm-up bias), reporting cold vs warm.
#
# Local testing uses file:// URIs; on a host with S3 (and nanolance built with S3 support) pass s3:// URIs.
#
# usage:
#   bench/blob_fetch_bench.sh --uris u1,u2,... [--sizes s1,s2,...] [--out DS] [--rounds N] \
#       [--gen build/nlance_blobgen] [--nlfetch build/nlance_blobfetch] [--pyfetch bench/fetch_pylance.py]
set -uo pipefail

URIS=""; SIZES=""; OUT=""; ROUNDS=3
GEN="build/nlance_blobgen"; NLFETCH="build/nlance_blobfetch"; PYFETCH="bench/fetch_pylance.py"
PYTHON="${PYTHON:-python}"

while [ $# -gt 0 ]; do
  case "$1" in
    --uris) URIS="$2"; shift 2;;
    --sizes) SIZES="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --rounds) ROUNDS="$2"; shift 2;;
    --gen) GEN="$2"; shift 2;;
    --nlfetch) NLFETCH="$2"; shift 2;;
    --pyfetch) PYFETCH="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[ -n "$URIS" ] || { echo "error: --uris required" >&2; exit 2; }
for b in "$GEN" "$NLFETCH"; do [ -x "$b" ] || command -v "$b" >/dev/null || { echo "error: not found: $b" >&2; exit 2; }; done

IFS=',' read -r -a URI_ARR <<< "$URIS"

# --- 1. resolve object sizes (unless supplied) ---
if [ -z "$SIZES" ]; then
  size_list=()
  for u in "${URI_ARR[@]}"; do
    case "$u" in
      s3://*)
        rest="${u#s3://}"; bucket="${rest%%/*}"; key="${rest#*/}"
        sz=$(aws s3api head-object --bucket "$bucket" --key "$key" --query ContentLength --output text 2>/dev/null) \
          || { echo "error: aws head-object failed for $u (need awscli + creds/endpoint)" >&2; exit 2; }
        ;;
      file://*) p="${u#file://}"; p="${p#/}"; sz=$(stat -c%s "/$p" 2>/dev/null || stat -c%s "$p");;
      *) sz=$(stat -c%s "$u");;
    esac
    [ -n "$sz" ] || { echo "error: could not size $u" >&2; exit 2; }
    size_list+=("$sz")
  done
  SIZES=$(IFS=,; echo "${size_list[*]}")
fi
echo "sizes: $SIZES"

# --- 2. build the tiled external-blob dataset ---
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
[ -n "$OUT" ] || OUT="$TMP/blobs.lance"
echo ">> generating dataset: $OUT"
"$GEN" --out "$OUT" --uris "$URIS" --sizes "$SIZES" || { echo "generator failed" >&2; exit 1; }

run_nl(){ "$NLFETCH" "$OUT" > "$1" 2> "$1.err"; return $?; }
run_py(){ "$PYTHON" "$PYFETCH" "$OUT" > "$1" 2> "$1.err"; return $?; }

# total fetch time (ms) = sum of per-blob nanos; also report blob count.
total_ms(){ awk -F'\t' '{s+=$3} END{printf "%.2f", s/1e6}' "$1"; }
nblobs(){ wc -l < "$1" | tr -d ' '; }

# md5 agreement: sort both by index, compare index+md5 columns.
md5_agree(){
  join -t$'\t' <(sort -k1,1n "$1" | cut -f1,2) <(sort -k1,1n "$2" | cut -f1,2) \
    | awk -F'\t' '$2!=$3{print "MISMATCH idx "$1": "$2" != "$3; bad=1} END{exit bad+0}'
}

echo "=================================================================="
echo "  blob-fetch benchmark   objects=${#URI_ARR[@]}   rounds=$ROUNDS"
echo "=================================================================="

# --- 3. cold phase: pylance x3 ---
PY_REF="$TMP/py_ref.tsv"
echo "-- cold: pylance x3 --"
for i in 1 2 3; do
  out="$TMP/py_cold_$i.tsv"
  if ! run_py "$out"; then
    rc=$?
    if [ "$rc" = "77" ]; then echo "pylance unavailable -> skip (77)"; exit 77; fi
    echo "pylance run failed:"; cat "$out.err" >&2; exit 1
  fi
  printf "   pylance cold #%d : %8s ms  (%s blobs)\n" "$i" "$(total_ms "$out")" "$(nblobs "$out")"
  cp "$out" "$PY_REF"
done

# nanolance reference (for md5 cross-check)
NL_REF="$TMP/nl_ref.tsv"
run_nl "$NL_REF" || { echo "nanolance run failed:"; cat "$NL_REF.err" >&2; exit 1; }

echo "-- correctness: md5(nanolance) == md5(pylance) per index --"
if md5_agree "$NL_REF" "$PY_REF"; then
  echo "   OK: all $(nblobs "$NL_REF") blobs agree"
else
  echo "   FAIL: md5 mismatch between nanolance and pylance" >&2; exit 1
fi

# --- 4. warm rounds, rotating which impl goes first ---
echo "-- warm: $ROUNDS rotated rounds --"
printf "   %-7s | %-12s | %-12s\n" "round" "nanolance" "pylance"
printf "   %-7s-+-%-12s-+-%-12s\n" "-------" "------------" "------------"
nl_sum=0; py_sum=0
for r in $(seq 1 "$ROUNDS"); do
  nlo="$TMP/nl_$r.tsv"; pyo="$TMP/py_$r.tsv"
  if [ $((r % 2)) -eq 1 ]; then run_nl "$nlo"; run_py "$pyo"; else run_py "$pyo"; run_nl "$nlo"; fi
  # every round must still agree (determinism + correctness)
  md5_agree "$nlo" "$pyo" >/dev/null || { echo "   FAIL: round $r md5 mismatch" >&2; exit 1; }
  nl_ms=$(total_ms "$nlo"); py_ms=$(total_ms "$pyo")
  printf "   %-7s | %9s ms | %9s ms\n" "$r" "$nl_ms" "$py_ms"
  nl_sum=$(awk "BEGIN{print $nl_sum+$nl_ms}"); py_sum=$(awk "BEGIN{print $py_sum+$py_ms}")
done
printf "   %-7s | %9s ms | %9s ms\n" "mean" \
  "$(awk "BEGIN{printf \"%.2f\", $nl_sum/$ROUNDS}")" "$(awk "BEGIN{printf \"%.2f\", $py_sum/$ROUNDS}")"

echo "=================================================================="
echo "done. (lower ms = faster total fetch over all blobs)"
