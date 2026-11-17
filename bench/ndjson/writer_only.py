#!/usr/bin/env python3
"""Writer-only comparison: nanolance.write_table against pylance's write_dataset, on the same tables.

The end-to-end benchmark (run.sh) includes the baseline's Python routing, so its time is not a
Lance-versus-Lance comparison. This script takes the tables ndjson2lance writes, reads them into Arrow
once, and times only the writes, each in a fresh process under the same core limit.

Usage:
  writer_only.py prep TOOL INPUT RESULTS_JSONL TABLES_DIR
      Runs TOOL --no-compress once into a scratch directory under TABLES_DIR. Keeps the tables whose
      column set the baseline wrote (the base-keys record of RESULTS_JSONL, matched as run.sh matches
      them), reads each with pylance, and saves it as TABLES_DIR/tNN.arrow (Arrow IPC).
  writer_only.py run WORK_DIR NANOLANCE_PY TABLES_DIR RUNS CORES...
      For each core count and each run, starts one fresh process per writer (taskset and thread
      environment set to CORES), and prints the medians. Writes WORK_DIR/writer_only.jsonl.
  writer_only.py measure WRITER TABLES_DIR OUT_DIR NANOLANCE_PY
      One measurement in this process (used by run). Prints one JSON line.

Writers: nanolance-nocompress (nanolance.write_table compression=False), nanolance-zstd
(compression=True), pylance (lance.write_dataset data_storage_version="2.2"). Each writes the whole
table set into OUT_DIR. The timed region is the writes only; tables are loaded and output sizes read
outside it. The `detail` column size is pylance's DataStatistics (bytes_on_disk per top-level field,
summed over the tables that have a `detail` column) for each output, so both writers are measured by
the same reader. Peak thread count is sampled from /proc/self/status during the timed region.
"""

import argparse
import glob
import json
import os
import shutil
import statistics
import subprocess
import sys
import threading
import time

WRITERS = ("nanolance-nocompress", "nanolance-zstd", "pylance")


def load_tables(tables_dir):
    import pyarrow as pa

    return [
        (os.path.basename(p)[:-len(".arrow")], pa.ipc.open_file(p).read_all())
        for p in sorted(glob.glob(os.path.join(tables_dir, "*.arrow")))
    ]


def prep(args):
    import lance

    results = [json.loads(line) for line in open(args.results_jsonl) if line.strip()]
    base = next(r for r in results if r["config"] == "base-keys")
    wanted = set(base["tables"])  # column-set keys the baseline wrote
    scratch = os.path.join(args.tables_dir, "tool-out")
    shutil.rmtree(scratch, ignore_errors=True)
    subprocess.run([args.tool, scratch, "-i", args.input, "--no-compress"], check=True,
                   stdout=subprocess.DEVNULL)
    kept = 0
    for name in sorted(os.listdir(scratch)):
        if not name.endswith(".lance"):
            continue
        d = lance.dataset(os.path.join(scratch, name))
        key = "|".join(sorted(f.name for f in d.schema))
        if key not in wanted:
            print(f"skip {name}: {d.count_rows()} rows, column set not written by the baseline")
            continue
        table = d.to_table()
        import pyarrow as pa

        with pa.OSFile(os.path.join(args.tables_dir, name[:-len(".lance")] + ".arrow"), "wb") as f:
            with pa.ipc.new_file(f, table.schema) as w:
                w.write_table(table)
        print(f"kept {name}: {table.num_rows} rows, {len(table.schema)} columns")
        kept += 1
    shutil.rmtree(scratch)
    print(f"prep: kept {kept} tables; baseline wrote {len(wanted)} column sets")


def measure(args):
    # Import nanolance from the private install, not from the source tree the editable install points at.
    sys.path[:] = [p for p in sys.path if "/home/user/nanolance" not in p]
    sys.path.insert(0, args.nanolance_py)
    import lance
    import nanolance
    import pyarrow as pa

    tables = load_tables(args.tables_dir)
    os.makedirs(args.out_dir, exist_ok=True)

    peak = [0]
    stop = threading.Event()

    def sample():
        while not stop.is_set():
            with open("/proc/self/status") as f:
                for line in f:
                    if line.startswith("Threads:"):
                        peak[0] = max(peak[0], int(line.split()[1]))
            time.sleep(0.005)

    sampler = threading.Thread(target=sample, daemon=True)
    sampler.start()
    load = open("/proc/loadavg").read().split()[:3]
    t0 = time.perf_counter()
    for name, table in tables:
        path = os.path.join(args.out_dir, name + ".lance")
        if args.writer == "nanolance-nocompress":
            nanolance.write_table(table, path, compression=False)
        elif args.writer == "nanolance-zstd":
            nanolance.write_table(table, path, compression=True)
        elif args.writer == "pylance":
            lance.write_dataset(table, path, data_storage_version="2.2")
        else:
            sys.exit(f"unknown writer {args.writer}")
    wall = time.perf_counter() - t0
    stop.set()
    sampler.join()

    out_bytes = sum(
        os.path.getsize(os.path.join(d, f))
        for d, _, fs in os.walk(args.out_dir) for f in fs
    )
    detail_bytes = detail_rows = 0
    for name, _ in tables:
        ds = lance.dataset(os.path.join(args.out_dir, name + ".lance"))
        if "detail" not in ds.schema.names:
            continue
        fid = ds.lance_schema.field("detail").id()  # Lance field ids count nested fields too
        stat = next(s for s in ds.stats.data_stats().fields if s.id == fid)
        detail_bytes += stat.bytes_on_disk
        detail_rows += ds.count_rows()
    print(json.dumps(dict(
        writer=args.writer, wall=wall, out_bytes=out_bytes, detail_bytes=detail_bytes,
        detail_rows=detail_rows, peak_threads=peak[0], load=load,
        rows=sum(t.num_rows for _, t in tables), tables=len(tables),
    )))


def run(args):
    cores_list = args.cores
    recs = []
    for run_no in range(1, args.runs + 1):
        for cores in cores_list:
            for writer in WRITERS:
                out_dir = os.path.join(args.work_dir, "out")
                shutil.rmtree(out_dir, ignore_errors=True)
                n = str(cores)
                cmd = [
                    "taskset", "-c", f"0-{cores - 1}",
                    "env", f"NANOLANCE_THREADS={n}", f"LANCE_CPU_THREADS={n}",
                    f"LANCE_IO_THREADS={n}", f"RAYON_NUM_THREADS={n}", f"OMP_NUM_THREADS={n}",
                    "python3", "-I", os.path.abspath(__file__), "measure", writer,
                    args.tables_dir, out_dir, args.nanolance_py,
                ]
                p = subprocess.run(cmd, capture_output=True, text=True)
                if p.returncode != 0:
                    sys.exit(f"{writer} cores={cores} failed:\n{p.stderr}")
                rec = json.loads(p.stdout.strip().splitlines()[-1])
                rec.update(cores=cores, run=run_no)
                recs.append(rec)
                shutil.rmtree(out_dir, ignore_errors=True)
                print(f"cores={cores} run={run_no} {writer}: write {rec['wall']:.3f}s "
                      f"out {rec['out_bytes'] / 1e6:.1f}MB peak_threads={rec['peak_threads']} "
                      f"load={rec['load'][0]}", flush=True)
    with open(os.path.join(args.work_dir, "writer_only.jsonl"), "w") as f:
        for r in recs:
            f.write(json.dumps(r) + "\n")

    print()
    print(f"{'cores':>5s} {'writer':22s} {'write s':>8s} {'out MB':>8s} {'detail B/row':>13s} "
          f"{'peak thr':>9s} {'load1':>6s} {'runs':>5s}")
    for cores in cores_list:
        for writer in WRITERS:
            rs = [r for r in recs if r["cores"] == cores and r["writer"] == writer]
            wall = statistics.median(r["wall"] for r in rs)
            out = statistics.median(r["out_bytes"] for r in rs)
            det = statistics.median(r["detail_bytes"] / r["detail_rows"] for r in rs)
            thr = max(r["peak_threads"] for r in rs)
            load = statistics.median(float(r["load"][0]) for r in rs)
            print(f"{cores:5d} {writer:22s} {wall:8.3f} {out / 1e6:8.1f} {det:13.1f} "
                  f"{thr:9d} {load:6.2f} {len(rs):5d}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("prep")
    p.add_argument("tool")
    p.add_argument("input")
    p.add_argument("results_jsonl")
    p.add_argument("tables_dir")
    p.set_defaults(fn=prep)

    p = sub.add_parser("run")
    p.add_argument("work_dir")
    p.add_argument("nanolance_py")
    p.add_argument("tables_dir")
    p.add_argument("runs", type=int)
    p.add_argument("cores", type=int, nargs="+")
    p.set_defaults(fn=run)

    p = sub.add_parser("measure")
    p.add_argument("writer", choices=WRITERS)
    p.add_argument("tables_dir")
    p.add_argument("out_dir")
    p.add_argument("nanolance_py")
    p.set_defaults(fn=measure)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
