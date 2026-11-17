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
#   CORES=N      cores for every timed command (default: nproc). Each timed command runs under
#                taskset -c 0-(N-1); the tool gets --threads N and NANOLANCE_THREADS=N; the baseline
#                gets RAYON_NUM_THREADS, LANCE_CPU_THREADS, LANCE_IO_THREADS, OMP_NUM_THREADS = N and
#                pyarrow.set_cpu_count / set_io_thread_count = N (baseline.py reads CORES)
#   MODES=keys   "keys" runs key mode only (no event-mode configs and no --threads 4 config);
#                default "keys event"
#   GH_GZ=FILE   a GH Archive hour (.json.gz); the GH section runs only when set
#
# Configurations: ndjson2lance in key mode (string columns zstd-compressed by default), --route-key
# event, key mode with --threads 4, key mode with --no-compress, and the baseline in key and event
# modes; with GH_GZ, both tools with
# --route-key type. Each run writes its output to WORKDIR/out/, measures it, and deletes it.
#
# Assertions (exit 1 if any fails):
#   A. each side accounts for every non-blank input line: rows + rejects + unwritable
#   B. for every table both sides wrote (tables matched by their set of column names), the row
#      counts are equal (the sorted row counts of that set must match)
#   C. ndjson2lance with --threads 4 writes the same tables and rows as with one thread
# The known difference (tables and rows the baseline cannot write) is printed as INFO only.
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
CORES="${CORES:-$(nproc)}"
MODES="${MODES:-keys event}"
CPUSET="0-$((CORES - 1))"
GEN="$ROOT/examples/ndjson2lance/tests/gen_logs.py"
BASE="$HERE/baseline.py"
LOGS="$WORK/logs"
RES="$WORK/results.jsonl"
mkdir -p "$WORK/out" "$LOGS"
: > "$RES"

echo "machine: nproc=$(nproc) cpu='$(lscpu | sed -n 's/^Model name: *//p')' load_before_start=$(cut -d' ' -f1-3 /proc/loadavg)"
echo "tool: $TOOL"
echo "workdir: $WORK"
echo "cores: $CORES (taskset -c $CPUSET; modes: $MODES)"

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
# the 1-minute load average read just before the start, and the bytes under OUTDIR. After the timing
# it lists the Lance tables in OUTDIR as {sorted column names: [row counts]}. One JSON line is appended
# to $RES. The output directory is deleted afterwards.
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
tables = {}
if os.path.isdir(out):
    out_bytes = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(out) for f in fs)
    import lance  # after the timing: reads the output's schemas and row counts
    for name in sorted(os.listdir(out)):
        if name.startswith("_") or not name.endswith(".lance"):
            continue
        try:
            d = lance.dataset(os.path.join(out, name))
        except ValueError:
            continue  # a directory a failed write left behind, not a dataset
        key = "|".join(sorted(f.name for f in d.schema))
        tables.setdefault(key, []).append(d.count_rows())
with open(inpath, "rb") as f:
    nonblank = sum(1 for line in f if line.strip())  # after the timing: the input's non-blank lines
rec = dict(config=config, run=int(run), kind=kind, route=route, threads=int(threads),
           input=inpath, nonblank=nonblank, in_bytes=os.path.getsize(inpath), wall=wall, rss_kb=rss_kb, rc=rc,
           out_bytes=out_bytes, load=load, log=log, tables=tables)
with open(res, "a") as f:
    f.write(json.dumps(rec) + "\n")
print(f"{config} run {run}: rc={rc} wall={wall:.3f}s rss={rss_kb / 1024:.0f}MB out={out_bytes}B load={load[0]}")
PY
    rm -rf -- "${out:?}"
}

# name kind route threads input extra-flags ("-" for none)
configs=(
    "tool-keys tool keys $CORES $IN -"
    "base-keys base keys $CORES $IN -"
    "tool-keys-nocompress tool keys $CORES $IN --no-compress"
)
if [[ " $MODES " == *" event "* ]]; then
    configs+=(
        "tool-event tool event $CORES $IN -"
        "base-event base event $CORES $IN -"
        "tool-keys-t4 tool keys 4 $IN -"
    )
fi
GHIN=""
if [[ -n "${GH_GZ:-}" ]]; then
    GHIN="$WORK/gh.ndjson"
    gunzip -c "$GH_GZ" > "$GHIN"
    configs+=(
        "gh-tool-type tool type $CORES $GHIN -"
        "gh-base-type base type $CORES $GHIN -"
    )
fi

for spec in "${configs[@]}"; do
    read -r config kind route threads inpath extra <<< "$spec"
    for run in $(seq 1 "$RUNS"); do
        out="$WORK/out/$config-$run"
        case "$kind" in
            tool)
                args=(env NANOLANCE_THREADS="$threads" taskset -c "$CPUSET"
                      "$TOOL" "$out" -i "$inpath" --threads "$threads")
                [[ "$route" == keys ]] || args+=(--route-key "$route")
                [[ "$extra" == "-" ]] || args+=($extra)
                ;;
            base)
                args=(env RAYON_NUM_THREADS="$threads" LANCE_CPU_THREADS="$threads"
                      LANCE_IO_THREADS="$threads" OMP_NUM_THREADS="$threads" CORES="$threads"
                      taskset -c "$CPUSET" python3 "$BASE" "$inpath" "$out")
                [[ "$route" == keys ]] || args+=(--route-key "$route")
                ;;
        esac
        measure "$config" "$run" "$kind" "$route" "$threads" "$inpath" "$out" "${args[@]}"
    done
done
rm -f -- "$GHIN"

echo
echo "results: CORES=$CORES"
python3 - "$RES" "$RUNS" <<'PY'
import json, re, statistics, sys
from collections import defaultdict

recs = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
runs = sys.argv[2]
by = defaultdict(list)
for r in recs:
    by[r["config"]].append(r)

def counts(rec):
    """(tables, rows, rejects, unwritable) from the tool's or the baseline's summary line."""
    text = open(rec["log"], encoding="utf-8", errors="replace").read()
    m = re.search(r"baseline: tables=(\d+) rows=(\d+) rejects=(\d+) unwritable=(\d+)", text)
    if not m:
        m = re.search(r"(\d+) tables, (\d+) rows, (\d+) rejects, \d+ new-key lines, (\d+) unwritable", text)
    return tuple(int(x) for x in m.groups()) if m else (None, None, None, None)

print(f"{'config':18s} {'wall s':>8s} {'in MB/s':>8s} {'rows/s':>10s} {'peak RSS MB':>12s} "
      f"{'out MB':>8s} {'out/in':>7s} {'tables':>7s} {'rows':>8s} {'rejects':>8s} {'unwritable':>11s} "
      f"{'rc':>3s} {'load1':>6s}")
summary = {}
for config in by:
    rs = sorted(by[config], key=lambda r: r["run"])
    c = [counts(r) for r in rs]
    tables, rows, rejects, unw = c[0]
    wall = statistics.median([r["wall"] for r in rs])
    inb = rs[0]["in_bytes"]
    outb = statistics.median([r["out_bytes"] for r in rs])
    rate = inb / 1e6 / wall
    rps = (rows or 0) / wall
    rss = statistics.median([r["rss_kb"] for r in rs]) / 1024
    load = statistics.median([float(r["load"][0]) for r in rs])
    rc = max(r["rc"] for r in rs)
    summary[config] = dict(
        input=rs[0]["input"], nonblank=rs[0]["nonblank"], counts=c, tables=rs[0]["tables"],
        # the same counts and the same per-table row counts in every run of this configuration
        stable=all(x == c[0] for x in c) and all(r["tables"] == rs[0]["tables"] for r in rs))
    print(f"{config:18s} {wall:8.3f} {rate:8.1f} {rps:10.0f} {rss:12.1f} {outb / 1e6:8.1f} "
          f"{outb / inb:7.3f} {tables!s:>7s} {rows!s:>8s} {rejects!s:>8s} {unw!s:>11s} {rc:>3d} {load:6.2f}")

print()
print(f"medians of {runs} runs per configuration; MB = 1e6 bytes; input MB/s uses the input file size")
print()

failed = False
def check(ok, text):
    global failed
    failed = failed or not ok
    print(("PASS " if ok else "FAIL ") + text)

# A. every non-blank input line is accounted for on each side
for config, s in summary.items():
    tables, rows, rejects, unw = s["counts"][0]
    total = s["nonblank"]
    check(rows + rejects + unw == total and s["stable"],
          f"A {config}: rows {rows} + rejects {rejects} + unwritable {unw} = {rows + rejects + unw} "
          f"vs {total} non-blank input lines")

# B. for every table both sides wrote (matched by column set), equal row counts
def table_rows(config):
    rs = sorted(by[config], key=lambda r: r["run"])
    return rs[0]["tables"]

def compare_tables(tool, base):
    """Each table the baseline wrote must match a tool table: the same column set and row count.

    Tables are matched by column set, since the two sides name them differently. A column set can
    hold several tables (--route-key type), so the baseline's row counts for a column set must be a
    sub-multiset of the tool's. Tool tables the baseline did not write are INFO, not failures."""
    from collections import Counter
    t, b = table_rows(tool), table_rows(base)
    for key in sorted(set(b)):
        ok = not (Counter(b[key]) - Counter(t.get(key, [])))
        check(ok, f"B {base} table with {len(key.split('|'))} columns (first: {key.split('|')[0]}): "
                  f"rows {sorted(b[key])} must all appear in {tool}: {sorted(t.get(key, []))}")
    for key in sorted(set(t)):
        missing = Counter(t[key]) - Counter(b.get(key, []))
        if missing:
            print(f"INFO {tool} tables {base} did not write: {len(key.split('|'))} columns, "
                  f"rows {sorted(missing.elements())} (sum {sum(missing.elements())})")

pairs = [("tool-keys", "base-keys"), ("tool-event", "base-event")]
if "gh-tool-type" in summary:
    pairs.append(("gh-tool-type", "gh-base-type"))
for tool, base in pairs:
    if tool in summary and base in summary:
        compare_tables(tool, base)

# B2. the tool without compression writes the same tables and rows as the default tool
if "tool-keys-nocompress" in summary:
    t, c = table_rows("tool-keys"), table_rows("tool-keys-nocompress")
    check(t == c, f"B tool-keys vs tool-keys-nocompress: same tables and per-table row counts")

# C. one thread and four threads write the same tables and rows
if "tool-keys-t4" in summary:
    t1, t4 = table_rows("tool-keys"), table_rows("tool-keys-t4")
    check(t1 == t4, f"C tool-keys vs tool-keys-t4 (--threads 4): same tables and per-table row counts")

# INFO: the baseline's unwritable group, from its summary line
for config in ("base-keys", "base-event", "gh-base-type"):
    if config in summary:
        tables, rows, rejects, unw = summary[config]["counts"][0]
        print(f"INFO {config}: unwritable {unw} lines (a group whose column type changes from number to "
              f"string, which pyarrow's whole-group inference cannot write; see README)")

sys.exit(1 if failed else 0)
PY
