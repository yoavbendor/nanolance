#!/usr/bin/env python3
"""Three-way benchmark: nanolance vs Rust Lance vs Parquet (file size + write/read time)."""
import os, glob, json, shutil, subprocess, tempfile, time, statistics
import pyarrow as pa, pyarrow.ipc as ipc, pyarrow.parquet as pq
import lance

# Cross-platform paths: build dir relative to repo root (override with NL_BUILD), scratch in BENCH_TMP.
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_BUILD = os.environ.get("NL_BUILD", os.path.join(_ROOT, "build"))
_EXT = ".exe" if os.name == "nt" else ""
EXE = os.path.join(_BUILD, "arrowipc2lance" + _EXT)
NLBENCH = os.path.join(_BUILD, "nlbench" + _EXT)
TMP = os.environ.get("BENCH_TMP", os.path.join(tempfile.gettempdir(), "nlbench"))
READ_ITERS = 7
os.makedirs(TMP, exist_ok=True)

def dsize(path_glob):
    return sum(os.path.getsize(f) for f in glob.glob(path_glob))

def best_read(fn, iters=READ_ITERS):
    ts = []
    for _ in range(iters):
        t0 = time.perf_counter(); fn(); ts.append((time.perf_counter()-t0)*1000)
    return min(ts)

def make_datasets():
    N = 200_000
    ds = {}
    # D1: pcap reference (columns model) — run-length URI, monotonic position, constant size
    MIN = N // 5000  # ~40 distinct, runs of 5000
    ds["pcap_ref"] = pa.table({
        "uri": pa.array([f"s3://my-bucket/captures/2026-06-09T12_{(i*MIN//N):02d}_00.pcapng" for i in range(N)], pa.string()),
        "position": pa.array([i*1500 for i in range(N)], pa.uint64()),
        "size": pa.array([1500]*N, pa.uint64()),
    })
    # D2: wide integer EPB-like — bitpack/RLE friendly
    import random; random.seed(1)
    ds["wide_int"] = pa.table({
        "ts": pa.array([1_700_000_000_000_000 + i*1500 + random.randint(0,200) for i in range(N)], pa.uint64()),
        "caplen": pa.array([random.randint(60, 1514) for _ in range(N)], pa.uint32()),
        "iface": pa.array([ (i//10000) % 4 for i in range(N)], pa.uint8()),   # low-card runs
        "ipproto": pa.array([random.choice([6,17,6,6,1]) for _ in range(N)], pa.uint8()),  # low-card scattered
    })
    # D3: high-cardinality — random unique-ish strings + random ints (where zstd-only lags)
    ds["high_card"] = pa.table({
        "id": pa.array([random.getrandbits(64) for _ in range(N)], pa.uint64()),
        "label": pa.array([f"obj-{random.getrandbits(40):010x}" for _ in range(N)], pa.string()),
    })
    # D4: float/double — byte-stream-split + zstd path (Phase B). A smooth signal (not random) so
    # byte-transpose actually helps zstd, the shape stock Lance itself targets with ByteStreamSplit.
    import math
    ds["float_smooth"] = pa.table({
        "reading": pa.array([math.sin(i * 0.001) * 1000.0 for i in range(N)], pa.float64()),
        "gain": pa.array([math.cos(i * 0.0007) * 10.0 for i in range(N)], pa.float32()),
    })
    # D5: bool — 1-bit-per-value packing path (Phase C). Mixed true/false, not constant/run-length, so
    # neither ConstantLayout nor RLE short-circuits it and the bitpack path is actually exercised.
    ds["bool_flags"] = pa.table({
        "flag": pa.array([random.random() < 0.37 for _ in range(N)], pa.bool_()),
    })
    # Fairness override for the rust-lance write of float_smooth: nanolance runs with --compress
    # (byte-stream-split + zstd on float/double), but rust lance's DEFAULT leaves floats essentially
    # uncompressed (~raw 12 B/row) — so without this, the bench compared our compressed write time
    # against rust's uncompressed one. Rust only applies BSS+zstd when BOTH field-metadata hints are
    # set (verified against lance 8.0.0), which is what this per-field metadata does. Only the rust
    # write uses this table; nanolance/parquet still consume the plain `tbl` (the metadata keys would
    # otherwise be inert hints carried through the Arrow IPC schema).
    bss_md = {b"lance-encoding:compression": b"zstd", b"lance-encoding:bss": b"on"}
    fs = ds["float_smooth"]
    rust_overrides = {
        "float_smooth": pa.Table.from_arrays(
            [fs.column(0), fs.column(1)],
            schema=pa.schema([
                pa.field("reading", pa.float64(), nullable=False, metadata=bss_md),
                pa.field("gain", pa.float32(), nullable=False, metadata=bss_md),
            ]),
        )
    }
    return ds, N, rust_overrides

WRITE_ITERS = 5

def best_of(fn, iters):
    return min(fn() for _ in range(iters))

def run_one(name, tbl, N, rust_tbl=None):
    print(f"\n========== {name}  ({N} rows, {tbl.num_columns} cols) ==========")
    arrow_path = f"{TMP}/{name}.arrow"
    with ipc.new_stream(arrow_path, tbl.schema) as w:
        w.write_table(tbl)
    if rust_tbl is None:
        rust_tbl = tbl

    rows = []

    # ---- Parquet (zstd) ----  write is in-process, so core==proc
    pqf = f"{TMP}/{name}.parquet"
    def w_pq():
        if os.path.exists(pqf): os.remove(pqf)
        t0=time.perf_counter(); pq.write_table(tbl, pqf, compression="zstd"); return (time.perf_counter()-t0)*1000
    wp = best_of(w_pq, WRITE_ITERS)
    szp = os.path.getsize(pqf)
    rp = best_read(lambda: pq.read_table(pqf))
    assert pq.read_table(pqf).num_rows == N
    rows.append(("parquet (zstd)", wp, wp, szp, rp, None))

    # ---- Rust Lance (2.2) ----  in-process, core==proc
    lf = f"{TMP}/{name}_lance.lance"
    def w_lance():
        shutil.rmtree(lf, ignore_errors=True)
        t0=time.perf_counter(); lance.write_dataset(rust_tbl, lf, mode="create", data_storage_version="2.2"); return (time.perf_counter()-t0)*1000
    wl = best_of(w_lance, WRITE_ITERS)
    szl = dsize(lf+"/data/*.lance")
    rl = best_read(lambda: lance.dataset(lf).to_table())
    assert lance.dataset(lf).to_table().num_rows == N
    rows.append(("rust lance", wl, wl, szl, rl, None))

    # ---- nanolance (--compress) ----  separate subprocess-total (proc) from core ingest+encode+commit
    nf = f"{TMP}/{name}_nl.lance"
    def w_nl():
        shutil.rmtree(nf, ignore_errors=True)
        with open(arrow_path, "rb") as fin:
            t0=time.perf_counter()
            r = subprocess.run([EXE, "-c", "--ignore-nullability", "--compress", "-o", nf],
                               stdin=fin, capture_output=True, text=True, check=True)
            proc = (time.perf_counter()-t0)*1000
        core = next((float(l.split("=")[1]) for l in r.stderr.splitlines() if l.startswith("nl_write_ms=")), float("nan"))
        return (proc, core)
    nl_runs = [w_nl() for _ in range(WRITE_ITERS)]
    wn_proc = min(p for p, _ in nl_runs)
    wn_core = min(c for _, c in nl_runs)
    szn = dsize(nf+"/data/*.lance")
    out = subprocess.run([NLBENCH, nf, str(READ_ITERS)], capture_output=True, text=True, check=True)
    rn_native = json.loads(out.stdout)["best_ms"]
    rn_lance = best_read(lambda: lance.dataset(nf).to_table())
    assert lance.dataset(nf).to_table().num_rows == N, "lance row count on nanolance file"
    rows.append(("nanolance", wn_core, wn_proc, szn, rn_native, rn_lance))

    # ---- report ----
    print(f"{'engine':16} {'write(core)':>11} {'write(proc)':>11} {'B/row':>8} {'read ms':>9} {'read(lance)':>12}")
    base = next(r[3] for r in rows if r[0]=='parquet (zstd)')
    for eng, wcore, wproc, sz, rms, rms2 in rows:
        ratio = f"{sz/base:.2f}x" if sz else ""
        print(f"{eng:16} {wcore:11.2f} {wproc:11.2f} {sz/N:8.3f} {rms:9.2f} "
              f"{('' if rms2 is None else f'{rms2:9.2f}'):>12}   ({ratio} vs pq)")
    return rows

def main():
    ds, N, rust_overrides = make_datasets()
    for name, tbl in ds.items():
        run_one(name, tbl, N, rust_overrides.get(name))
    print(f"\nnote: best of {WRITE_ITERS} writes / {READ_ITERS} reads. write(core)=in-process encode work "
          "(parquet/lance: the write call; nanolance: ingest+encode+commit, EXCLUDING process startup + "
          "Arrow-IPC parse). write(proc)=full wall clock (nanolance includes subprocess startup + IPC parse). "
          "read ms=native reader; read(lance)=rust-lance reading the nanolance file.")

if __name__ == "__main__":
    main()
