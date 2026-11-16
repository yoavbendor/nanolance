#!/usr/bin/env bash
# Benchmark ndjson2lance against the plain pyarrow.json + pylance baseline (baseline.py).
#
# Usage: bench/ndjson/run.sh [TOOL [WORKDIR]]
#   TOOL     path to the ndjson2lance binary (default: $NDJSON2LANCE)
#   WORKDIR  scratch directory for the input, outputs, logs and results
#            (default: a new directory under $TMPDIR)
#
# Environment:
#   INPUT=FILE   existing realistic NDJSON to use; without it the input is generated
#   SEED=2       generator seed            LINES=900000  generator --lines (about 1 GB)
#   RUNS=3       runs per configuration (the table shows medians)
#   GH_GZ=FILE   a GH Archive hour (.json.gz); the GH section runs only when set
#
# Configurations: ndjson2lance key mode, --route-key event, key mode with --threads 4, and the
# baseline in key and event modes; with GH_GZ, both tools with --route-key type. Each run writes its
# output to WORKDIR/out/, measures it, and deletes it. Nothing outside WORKDIR is written.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TOOL="${1:-${NDJSON2LANCE:-}}"
if [[ -z "$TOOL" || ! -x "$TOOL" ]]; then
    echo "run.sh: give the ndjson2lance binary as the first argument or set NDJSON2LANCE" >&2
    exit 2
fi
WORK="${2:-$(mktemp -d "${TMPDIR:-/tmp}/ndjson-bench.XXXXXX")}"
RUNS="${RUNS:-3}"
GEN="$ROOT/examples/ndjson2lance/tests/gen_logs.py"
BASE="$HERE/baseline.py"
LOGS="$WORK/logs"
RES="$WORK/results.jsonl"
mkdir -p "$WORK/out" "$LOGS"
: > "$RES"

echo "machine: nproc=$(nproc) cpu='$(lscpu | sed -n 's/^Model name: *//p')' load_before_start=$(cut -d' ' -f1-3 /proc/loadavg)"
echo "tool: $TOOL"
echo "workdir: $WORK"

if [[ -n "${INPUT:-}" ]]; then
    IN="$INPUT"
    echo "input: existing $IN"
else
    IN="$WORK/in.ndjson"
    echo "input: generating realistic seed=${SEED:-2} lines=${LINES:-900000} -> $IN"
    python3 -I "$GEN" --profile realistic --lines "${LINES:-900000}" --seed "${SEED:-2}" \
        --out "$IN" --truth "$WORK/truth.json"
fi

# measure CONFIG RUN KIND ROUTE THREADS INPUT OUTDIR CMD...
# Runs CMD in a fresh python process, which reports wall time, peak RSS of the child (RUSAGE_CHILDREN),
# the 1-minute load average read just before the start, and the bytes under OUTDIR; appends one JSON
# line to $RES. The output directory is deleted after the measurement.
measure() {
    local config=$1 run=$2 kind=$3 route=$4 threads=$5 inpath=$6 out=$7
    shift 7
    rm -rf -- "${out:?}"
    python3 - "$config" "$run" "$kind" "$route" "$threads" "$inpath" "$out" \
        "$LOGS/$config-$run.log" "$RES" "$@" <<'PY'
import json, os, resource, subprocess, sys, time
config, run, kind, route, threads, inpath, out, log, res, *cmd = sys.argv[1:]
load = open("/proc/loadavg").read().split()[:3]
with open(log, "wb") as lf:
    t0 = time.perf_counter()
    rc = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT).returncode
    wall = time.perf_counter() - t0
rss_kb = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
out_bytes = 0
if os.path.isdir(out):
    out_bytes = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(out) for f in fs)
rec = dict(config=config, run=int(run), kind=kind, route=route, threads=int(threads),
           in_bytes=os.path.getsize(inpath), wall=wall, rss_kb=rss_kb, rc=rc,
           out_bytes=out_bytes, load=load, log=log)
with open(res, "a") as f:
    f.write(json.dumps(rec) + "\n")
print(f"{config} run {run}: rc={rc} wall={wall:.3f}s rss={rss_kb / 1024:.0f}MB out={out_bytes}B load={load[0]}")
PY
    rm -rf -- "${out:?}"
}

configs=(
    "tool-keys tool keys 1 $IN"
    "base-keys base keys 1 $IN"
    "tool-event tool event 1 $IN"
    "base-event base event 1 $IN"
    "tool-keys-t4 tool keys 4 $IN"
)
GHIN=""
if [[ -n "${GH_GZ:-}" ]]; then
    GHIN="$WORK/gh.ndjson"
    gunzip -c "$GH_GZ" > "$GHIN"
    configs+=(
        "gh-tool-type tool type 1 $GHIN"
        "gh-base-type base type 1 $GHIN"
    )
fi

for spec in "${configs[@]}"; do
    read -r config kind route threads inpath <<< "$spec"
    for run in $(seq 1 "$RUNS"); do
        out="$WORK/out/$config-$run"
        case "$kind" in
            tool)
                args=("$TOOL" "$out" -i "$inpath" --threads "$threads")
                [[ "$route" == keys ]] || args+=(--route-key "$route")
                ;;
            base)
                args=(python3 "$BASE" "$inpath" "$out")
                [[ "$route" == keys ]] || args+=(--route-key "$route")
                ;;
        esac
        measure "$config" "$run" "$kind" "$route" "$threads" "$inpath" "$out" "${args[@]}"
    done
done
rm -f -- "$GHIN"

echo
python3 - "$RES" "$RUNS" <<'PY'
import json, re, statistics, sys
from collections import defaultdict

recs = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
by = defaultdict(list)
for r in recs:
    by[r["config"]].append(r)

def counts(rec):
    text = open(rec["log"], encoding="utf-8", errors="replace").read()
    m = re.search(r"baseline: tables=(\d+) rows=(\d+) rejects=(\d+) unwritable=(\d+)", text)
    if m:
        return tuple(int(x) for x in m.groups())
    m = re.search(r"(\d+) tables, (\d+) rows, (\d+) rejects, \d+ new-key lines, (\d+) unwritable", text)
    if m:
        return tuple(int(x) for x in m.groups())
    return (None, None, None, None)

def med(vals):
    return statistics.median(vals)

print(f"{'config':14s} {'wall s':>8s} {'in MB/s':>8s} {'rows/s':>10s} {'peak RSS MB':>12s} "
      f"{'out MB':>8s} {'out/in':>7s} {'tables':>7s} {'rows':>8s} {'rejects':>8s} {'unwritable':>11s} "
      f"{'rc':>3s} {'load1':>6s}")
summary = {}
for config in [c for c in by if True]:
    rs = sorted(by[config], key=lambda r: r["run"])
    c = [counts(r) for r in rs]
    tables, rows, rejects, unw = c[0]
    wall = med([r["wall"] for r in rs])
    inb = rs[0]["in_bytes"]
    outb = med([r["out_bytes"] for r in rs])
    rate = inb / 1e6 / wall
    rps = (rows or 0) / wall
    rss = med([r["rss_kb"] for r in rs]) / 1024
    load = med([float(r["load"][0]) for r in rs])
    rc = max(r["rc"] for r in rs)
    summary[config] = dict(tables=tables, rows=rows, rejects=rejects, unw=unw,
                           consistent=all(x == c[0] for x in c), runs=len(rs))
    print(f"{config:14s} {wall:8.3f} {rate:8.1f} {rps:10.0f} {rss:12.1f} {outb / 1e6:8.1f} "
          f"{outb / inb:7.3f} {tables!s:>7s} {rows!s:>8s} {rejects!s:>8s} {unw!s:>11s} {rc:>3d} {load:6.2f}")

print()
print(f"medians of {sys.argv[2]} runs per configuration; MB = 1e6 bytes; input MB/s uses the input file size")
bad = False
pairs = [("tool-keys", "base-keys"), ("tool-event", "base-event"), ("tool-keys", "tool-keys-t4")]
if "gh-tool-type" in summary:
    pairs.append(("gh-tool-type", "gh-base-type"))
for a, b in pairs:
    if a not in summary or b not in summary:
        continue
    sa, sb = summary[a], summary[b]
    for field in ("tables", "rows"):
        same = sa[field] == sb[field]
        bad |= not same
        verdict = "EQUAL" if same else "DIFFER"
        print(f"ASSERT {a} vs {b} {field}: {sa[field]} vs {sb[field]} {verdict}")
    if not (sa["consistent"] and sb["consistent"]):
        bad = True
        print(f"ASSERT {a} vs {b}: counts vary between runs")
sys.exit(1 if bad else 0)
PY
