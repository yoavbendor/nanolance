#!/usr/bin/env python3
"""The full benchmark matrix: nanolance (C++ and Python) against Rust Lance (pylance), with Parquet
as a familiar yardstick -- every writer, every reader, every file, across the Arrow types people store.

    python tools/bench_matrix.py                  # everything; writes bench/results/matrix.json
    python tools/bench_matrix.py --quick          # a tenth of the rows, for a smoke run
    python tools/bench_matrix.py --only list_int64 map_string_int64

WHAT IS MEASURED, per dataset (one column of one type, so a number can be attributed to it):

  write   nanolance-cpp   the C API, in-process: writer open + write_batch per batch + commit, repeated
                          in one process (tools/nlbench --write), pinned to one core
          nanolance-py    nanolance.write_table from Python
          rust-lance      lance.write_dataset (pylance 12, Lance format 2.2), all cores
          rust-lance-1c   the same, pinned to one core with one CPU and one I/O thread -- the
                          per-core comparison (nanolance-cpp runs pinned to one core as well)
          nanolance-cpp-mt  the same on all cores (nanolance's default thread count)
          nanolance-cpp-budget  the same with max_pending_bytes = 4 MiB, the edge-device setting
          rust-native     the lance crate itself (tools/lance_rs_bench, no Python), all cores
          rust-native-1c  the same pinned to one core, one tokio worker, one CPU and one I/O thread
          parquet         pyarrow.parquet.write_table, zstd -- the yardstick, not a competitor
  read    each Lance reader reads BOTH Lance files (the one nanolance wrote and the one Rust Lance
          wrote), so the matrix answers "can I mix them" as well as "how fast":
          nanolance-cpp   lance_table_read_dataset into Arrow arrays (tools/nlbench), pinned to one core
          nanolance-cpp-mt  the same on all cores
          nanolance-cpp-retain  the same with glibc told to keep freed memory
                          (MALLOC_MMAP_THRESHOLD_/MALLOC_TRIM_THRESHOLD_). By default glibc hands a
                          read's buffers back to the OS and the next read page-faults them in again;
                          Rust Lance and pyarrow use pooling allocators that do not. Two environment
                          variables (or linking jemalloc/mimalloc) give nanolance the same.
          nanolance-py    nanolance.read_table -> pyarrow.Table
          rust-lance      lance.dataset(path).to_table(), all cores
          rust-lance-1c   the same, pinned to one core
          rust-native(-1c)  lance_rs_bench: Dataset::open + scan into record batches, as above
          parquet         pyarrow.parquet.read_table of the Parquet file
  memory  peak_mb: for every native process (nlbench, lance_rs_bench), the most memory one run added
          over what the process held before it -- the input batches of a write, nothing for a read,
          whose peak includes the table it returns
  footprint  stripped binary sizes, the Python packages' native libraries, third-party code
  size    bytes on disk of each file

HOW: every timing is one warm-up run, then the MEDIAN of --runs runs (default 7), files in the page
cache -- a warm read, which is what repeated queries see. Every read is checked against the source
table once (values and types) before it is timed; a reader that returns something else is reported as
a failure, not a time. nanolance's reader and writer use one thread (the C++ runs are pinned to one
core too); Rust Lance runs twice, on all cores and pinned to one; pyarrow uses all cores. The
machine, versions and core count are recorded with the results.
"""

from __future__ import annotations

import argparse
import datetime as dt
import decimal
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.ipc as ipc
import pyarrow.parquet as pq

import lance
import nanolance

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("NL_BUILD", ROOT / "build"))
NLBENCH = BUILD / "nlbench"
# The pure-Rust counterpart (tools/lance_rs_bench, `cargo build --release` there). Optional: without
# it the rust-native columns are left out.
RUST_BENCH = Path(os.environ.get("LANCE_RS_BENCH", ROOT / "tools" / "lance_rs_bench" / "target" / "release" / "lance_rs_bench"))


# ── Datasets ─────────────────────────────────────────────────────────────────────────────────────

def _rng(seed):
    return np.random.default_rng(seed)


def _strings(values):
    return pa.array(values, pa.utf8())


WORDS = np.array("alpha beta gamma delta epsilon zeta theta kappa lambda sigma omega north south river "
                 "stone cloud field light storm quiet".split())


def _sentences(n, rng, lo=3, hi=12):
    counts = rng.integers(lo, hi, n)
    picks = rng.integers(0, len(WORDS), counts.sum())
    out, at = [], 0
    for i, c in enumerate(counts):
        out.append(" ".join(WORDS[picks[at:at + c]]) + f" {i}")
        at += c
    return out


def _random_blobs(k):
    lengths = 16 + np.arange(k) % 49
    offsets = np.zeros(k + 1, np.int32)
    np.cumsum(lengths, out=offsets[1:])
    data = _rng(13).bytes(int(offsets[-1]))
    return pa.Array.from_buffers(pa.binary(), k, [None, pa.py_buffer(offsets), pa.py_buffer(data)])


def _list(offsets_lengths, values, list_type):
    offsets = np.zeros(len(offsets_lengths) + 1, np.int32)
    np.cumsum(offsets_lengths, out=offsets[1:])
    return pa.ListArray.from_arrays(pa.array(offsets, pa.int32()), values).cast(list_type)


def build_datasets(scale: float):
    """name -> (category, description, rows, builder). Row counts are chosen so each dataset is
    tens of MB in memory: big enough to time, small enough to run the whole matrix in minutes."""

    def n(rows):
        return max(1000, int(rows * scale))

    D = {}

    def add(name, category, description, rows, build):
        D[name] = (category, description, n(rows), build)

    # Integers and floats.
    add("int64_ids", "numeric", "int64, increasing ids", 2_000_000,
        lambda k: pa.array(np.arange(k, dtype=np.int64) * 3 + 1_000_000))
    add("int32_small", "numeric", "int32, values 0-999", 2_000_000,
        lambda k: pa.array(_rng(1).integers(0, 1000, k, dtype=np.int32)))
    add("uint8_codes", "numeric", "uint8, 5 distinct codes", 2_000_000,
        lambda k: pa.array(_rng(2).choice(np.array([6, 17, 1, 58, 132], np.uint8), k)))
    add("int64_random", "numeric", "int64, random (incompressible)", 2_000_000,
        lambda k: pa.array(_rng(3).integers(-2**62, 2**62, k, dtype=np.int64)))
    add("float64_smooth", "numeric", "float64, smooth signal", 2_000_000,
        lambda k: pa.array(np.sin(np.arange(k) * 0.001) * 100.0))
    add("float32_random", "numeric", "float32, random", 2_000_000,
        lambda k: pa.array(_rng(4).standard_normal(k).astype(np.float32)))
    add("bool_flags", "numeric", "bool, 30% true", 2_000_000,
        lambda k: pa.array(_rng(5).random(k) < 0.3))
    # Temporal and decimal.
    add("timestamp_us", "temporal", "timestamp[us], 1.5 ms apart with jitter", 2_000_000,
        lambda k: pa.array(1_700_000_000_000_000 + np.arange(k, dtype=np.int64) * 1500
                           + _rng(6).integers(0, 200, k), pa.timestamp("us")))
    add("date32", "temporal", "date32, ten years of days", 2_000_000,
        lambda k: pa.array(_rng(7).integers(18000, 21650, k, dtype=np.int32), pa.int32()).cast(pa.date32()))
    add("decimal128", "temporal", "decimal128(18, 4) prices", 500_000,
        lambda k: pa.array([decimal.Decimal(int(v)).scaleb(-4) for v in _rng(8).integers(0, 10**9, k)],
                           pa.decimal128(18, 4)))
    # Strings and binary.
    add("string_lowcard", "string", "utf8, 8 distinct values (log levels)", 1_000_000,
        lambda k: _strings(np.array(["INFO", "WARN", "ERROR", "DEBUG", "TRACE", "FATAL", "AUDIT", "NOTICE"])
                           [_rng(9).integers(0, 8, k)].tolist()))
    add("string_ids", "string", "utf8, unique ids (~15 bytes)", 1_000_000,
        lambda k: _strings([f"obj-{v:010x}" for v in _rng(10).integers(0, 2**40, k)]))
    add("string_urls", "string", "utf8, URLs (~55 bytes)", 500_000,
        lambda k: _strings([f"https://example.com/users/{u}/items/{i}?ref=r{u % 13}"
                            for u, i in zip(_rng(11).integers(0, 10**6, k), range(k))]))
    add("string_text", "string", "utf8, sentences (~50 bytes)", 500_000,
        lambda k: _strings(_sentences(k, _rng(12))))
    add("string_long", "string", "utf8, ~1 KB documents", 50_000,
        lambda k: _strings([" ".join(_sentences(20, _rng(i))) for i in range(k)]))
    add("binary_blobs", "string", "binary, 16-64 random bytes", 500_000, _random_blobs)
    add("uuid_fsb16", "string", "fixed_size_binary(16), UUIDs", 1_000_000,
        lambda k: pa.FixedSizeBinaryArray.from_buffers(pa.binary(16), k,
                                                      [None, pa.py_buffer(_rng(14).bytes(16 * k))]))
    # Nulls.
    add("int64_nulls", "nullable", "int64, 10% null", 2_000_000,
        lambda k: pa.array(np.arange(k, dtype=np.int64), mask=_rng(15).random(k) < 0.1))
    add("string_nulls", "nullable", "utf8 sentences, 10% null", 500_000,
        lambda k: pa.array(_sentences(k, _rng(16)), mask=_rng(17).random(k) < 0.1))
    # Vectors and nested.
    add("vector_f32x128", "nested", "fixed_size_list<float32, 128> embeddings", 100_000,
        lambda k: pa.FixedSizeListArray.from_arrays(pa.array(_rng(18).standard_normal(k * 128).astype(np.float32)), 128))
    add("struct_mixed", "nested", "struct<id: int64, name: utf8, score: float64>", 500_000,
        lambda k: pa.StructArray.from_arrays(
            [pa.array(np.arange(k, dtype=np.int64)), _strings([f"user{i % 50_000}" for i in range(k)]),
             pa.array(_rng(19).random(k))], ["id", "name", "score"]))
    add("list_int64", "nested", "list<int64>, 0-8 items", 500_000,
        lambda k: _list(_rng(20).integers(0, 9, k), pa.array(_rng(21).integers(0, 10**6, 4 * k + 8 * k)),
                        pa.list_(pa.int64())))
    add("list_string", "nested", "list<utf8> tags, 0-5 items", 300_000,
        lambda k: _list(_rng(22).integers(0, 6, k),
                        _strings(np.array([f"tag{i}" for i in range(200)])[_rng(23).integers(0, 200, 5 * k)].tolist()),
                        pa.list_(pa.utf8())))
    add("map_string_int64", "nested", "map<utf8, int64>, 0-4 entries", 300_000,
        lambda k: pa.array([[(f"k{j}", i + j) for j in range(i % 5)] for i in range(k)], pa.map_(pa.utf8(), pa.int64())))
    add("list_struct", "nested", "list<struct<x: int32, label: utf8>>, 0-3 items", 200_000,
        lambda k: pa.array([[{"x": i + j, "label": f"l{(i + j) % 40}"} for j in range(i % 4)] for i in range(k)]))
    add("list_list_int32", "nested", "list<list<int32>>", 200_000,
        lambda k: pa.array([[[i + m for m in range(j % 3)] for j in range(i % 4)] for i in range(k)],
                           pa.list_(pa.list_(pa.int32()))))
    return D


def mixed_table(scale: float):
    """A realistic event table: most of the above side by side."""
    k = max(1000, int(300_000 * scale))
    rng = _rng(99)
    return pa.table({
        "event_id": pa.array(np.arange(k, dtype=np.int64)),
        "ts": pa.array(1_700_000_000_000_000 + np.arange(k, dtype=np.int64) * 1500, pa.timestamp("us")),
        "user": _strings([f"user-{v:08x}" for v in rng.integers(0, 2**32, k)]),
        "level": _strings(np.array(["INFO", "WARN", "ERROR", "DEBUG"])[rng.integers(0, 4, k)].tolist()),
        "latency_ms": pa.array(rng.gamma(2.0, 20.0, k)),
        "ok": pa.array(rng.random(k) < 0.97),
        "bytes": pa.array(rng.integers(0, 1_000_000, k, dtype=np.int64), mask=rng.random(k) < 0.05),
        "message": _strings(_sentences(k, rng)),
        "tags": _list(rng.integers(0, 4, k), _strings(np.array(["a", "b", "c", "prod", "beta"])[rng.integers(0, 5, 3 * k)].tolist()),
                      pa.list_(pa.utf8())),
        "geo": pa.StructArray.from_arrays([pa.array(rng.random(k) * 180 - 90), pa.array(rng.random(k) * 360 - 180)],
                                          ["lat", "lon"]),
    })


# ── Timing helpers ───────────────────────────────────────────────────────────────────────────────

def timed(fn, runs):
    fn()  # warm-up
    samples = []
    for _ in range(runs):
        t0 = time.perf_counter()
        fn()
        samples.append((time.perf_counter() - t0) * 1000)
    return statistics.median(samples)


def dir_bytes(path: Path) -> int:
    if path.is_file():
        return path.stat().st_size
    return sum(f.stat().st_size for f in (path / "data").glob("*"))


def same(got: pa.Table, want: pa.Table) -> bool:
    if got.num_rows != want.num_rows:
        return False
    got = got.combine_chunks()
    want = want.combine_chunks()
    if got.schema.types != want.schema.types:
        try:
            got = got.cast(want.schema)
        except (pa.ArrowInvalid, pa.ArrowNotImplementedError):
            return False
    return got.equals(want)


RETAINING_MALLOC = {"MALLOC_MMAP_THRESHOLD_": str(1 << 30), "MALLOC_TRIM_THRESHOLD_": str(1 << 30)}
ONE_CORE_ENV = {"LANCE_CPU_THREADS": "1", "LANCE_IO_THREADS": "1", "OMP_NUM_THREADS": "1"}
RUST_NATIVE_1C_ENV = {**ONE_CORE_ENV, "TOKIO_WORKER_THREADS": "1"}
BUDGET_BYTES = 4 << 20  # nanolance-cpp-budget: the writer's max_pending_bytes


def native(cmd, one_core=False, env=None):
    """Run a native bench tool (nlbench or lance_rs_bench) and return its JSON line."""
    full_env = {**os.environ, **(RUST_NATIVE_1C_ENV if one_core else {}), **(env or {})}
    r = subprocess.run(pinned(cmd) if one_core else cmd, capture_output=True, text=True, env=full_env)
    if r.returncode != 0:
        raise RuntimeError((r.stderr or r.stdout).strip()[-300:])
    return json.loads(r.stdout.strip().splitlines()[-1])


def pinned(cmd):
    """Run `cmd` on core 0 (Linux taskset); falls back to unpinned where taskset is missing."""
    return (["taskset", "-c", "0"] + cmd) if shutil.which("taskset") else cmd


def rust_one_core(action, path, runs, ipc_path=None):
    """Median ms of a Rust Lance read or write in a subprocess pinned to one core."""
    cmd = pinned([sys.executable, str(Path(__file__).resolve()), "--runs", str(runs), "--worker", action, str(path)]
                 + ([str(ipc_path)] if ipc_path else []))
    r = subprocess.run(cmd, capture_output=True, text=True, env={**os.environ, **ONE_CORE_ENV})
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip()[-300:])
    return float(r.stdout.strip().splitlines()[-1])


def worker(action, path, runs, ipc_path):
    path = Path(path)
    if action == "read":
        print(timed(lambda: lance.dataset(str(path)).to_table(), runs))
    else:
        table = ipc.open_stream(ipc_path).read_all()

        def write():
            shutil.rmtree(path, ignore_errors=True)
            lance.write_dataset(table, str(path), data_storage_version="2.2")
        print(timed(write, runs))


def run_one(name, table, runs, work: Path, verbose):
    rec = {"rows": table.num_rows, "arrow_bytes": table.nbytes, "write_ms": {}, "read_ms": {}, "size": {},
           "peak_mb": {}, "errors": {}}
    ipc_path = work / f"{name}.arrow"
    with ipc.new_stream(ipc_path, table.schema) as w:
        for batch in table.to_batches(max_chunksize=65536):
            w.write_batch(batch)
    nl_path, py_path, rust_path, pq_path = (work / f"{name}_nl.lance", work / f"{name}_nlpy.lance",
                                            work / f"{name}_rust.lance", work / f"{name}.parquet")

    # Writers. Every writer gets the same input: the table as 64K-row batches, the way a stream
    # arrives (and what the IPC file the C++ and one-core runs read holds).
    table = pa.Table.from_batches(table.to_batches(max_chunksize=65536))

    try:  # the first of the runs + 1 is the warm-up; warm_median_ms leaves it out
        out = native([str(NLBENCH), "--write", str(ipc_path), str(nl_path), str(runs + 1)], one_core=True)
        rec["write_ms"]["nanolance-cpp"] = out["warm_median_ms"]
        rec["peak_mb"]["nanolance-cpp write"] = out["peak_rss_mb"]
    except Exception as e:  # noqa: BLE001 -- recorded, not hidden
        rec["errors"]["write nanolance-cpp"] = str(e)[:300]

    # The same on every core (the process's CPUs; nanolance's default).
    mt_path = work / f"{name}_nlmt.lance"
    try:
        out = native([str(NLBENCH), "--write", str(ipc_path), str(mt_path), str(runs + 1)])
        rec["write_ms"]["nanolance-cpp-mt"] = out["warm_median_ms"]
        rec["peak_mb"]["nanolance-cpp-mt write"] = out["peak_rss_mb"]
        if not same(pa.table(nanolance.read_table(mt_path)), table):
            rec["errors"]["write nanolance-cpp-mt"] = "wrote different data"
    except Exception as e:  # noqa: BLE001
        rec["errors"]["write nanolance-cpp-mt"] = str(e)[:300]
    shutil.rmtree(mt_path, ignore_errors=True)

    # The same with the writer's memory budget (max_pending_bytes): it commits a fragment whenever
    # it holds 4 MiB -- what an edge device would set to bound its resident memory while saving.
    budget_path = work / f"{name}_nlbudget.lance"
    try:
        out = native([str(NLBENCH), "--write", str(ipc_path), str(budget_path), str(runs + 1),
                      "--budget", str(BUDGET_BYTES)], one_core=True)
        rec["write_ms"]["nanolance-cpp-budget"] = out["warm_median_ms"]
        rec["peak_mb"]["nanolance-cpp-budget write"] = out["peak_rss_mb"]
        if not same(pa.table(nanolance.read_table(budget_path)), table):
            rec["errors"]["write nanolance-cpp-budget"] = "wrote different data"
    except Exception as e:  # noqa: BLE001
        rec["errors"]["write nanolance-cpp-budget"] = str(e)[:300]
    shutil.rmtree(budget_path, ignore_errors=True)

    # The lance crate itself, no Python: all cores, and pinned to one core with one thread per pool.
    rsn_path = work / f"{name}_rsn.lance"
    if RUST_BENCH.exists():
        for label, one_core in (("rust-native", False), ("rust-native-1c", True)):
            try:
                out = native([str(RUST_BENCH), "write", str(ipc_path), str(rsn_path), str(runs + 1)], one_core=one_core)
                rec["write_ms"][label] = out["warm_median_ms"]
                rec["peak_mb"][f"{label} write"] = out["peak_rss_mb"]
            except Exception as e:  # noqa: BLE001
                rec["errors"][f"write {label}"] = str(e)[:300]
        try:  # what it wrote is checked like any read: through pylance, against the source
            if not same(lance.dataset(str(rsn_path)).to_table(), table):
                rec["errors"]["write rust-native"] = "wrote different data"
            else:
                rec["size"]["rust-native"] = dir_bytes(rsn_path)
        except Exception as e:  # noqa: BLE001
            rec["errors"]["write rust-native"] = str(e)[:300]
        shutil.rmtree(rsn_path, ignore_errors=True)

    def w_nl_py():
        shutil.rmtree(py_path, ignore_errors=True)
        nanolance.write_table(table, py_path)

    def w_rust():
        shutil.rmtree(rust_path, ignore_errors=True)
        lance.write_dataset(table, str(rust_path), data_storage_version="2.2")

    def w_pq():
        pq.write_table(table, pq_path, compression="zstd")

    for label, fn in (("nanolance-py", w_nl_py), ("rust-lance", w_rust), ("parquet", w_pq)):
        try:
            rec["write_ms"][label] = timed(fn, runs)
        except Exception as e:  # noqa: BLE001
            rec["errors"][f"write {label}"] = str(e)[:300]
    try:
        rec["write_ms"]["rust-lance-1c"] = rust_one_core("write", work / f"{name}_rust1c.lance", runs, ipc_path)
    except Exception as e:  # noqa: BLE001
        rec["errors"]["write rust-lance-1c"] = str(e)[:300]
    shutil.rmtree(work / f"{name}_rust1c.lance", ignore_errors=True)

    for label, path in (("nanolance", nl_path), ("rust-lance", rust_path), ("parquet", pq_path)):
        if path.exists():
            rec["size"][label] = dir_bytes(path)

    # Readers, over both Lance files.
    readers = {
        "nanolance-py": lambda p: pa.table(nanolance.read_table(p)),
        "rust-lance": lambda p: lance.dataset(str(p)).to_table(),
    }
    for file_label, path in (("nanolance", nl_path), ("rust-lance", rust_path)):
        if not path.exists():
            continue
        for reader, fn in readers.items():
            key = f"{reader} <- {file_label}"
            try:
                if not same(fn(path), table):
                    rec["errors"][f"read {key}"] = "returned different data"
                    continue
                rec["read_ms"][key] = timed(lambda: fn(path), runs)
            except Exception as e:  # noqa: BLE001
                rec["errors"][f"read {key}"] = str(e)[:300]
        key = f"nanolance-cpp <- {file_label}"
        try:
            # Correctness of the C++ read is the Python read's: both are lance_table_read_dataset.
            out = native([str(NLBENCH), str(path), str(runs + 1)], one_core=True)
            if out["rows"] != table.num_rows:
                raise RuntimeError(f"read {out['rows']} rows, expected {table.num_rows}")
            rec["read_ms"][key] = out["warm_median_ms"]
            rec["peak_mb"][f"nanolance-cpp read <- {file_label}"] = out["peak_rss_mb"]
            r = subprocess.run(pinned([str(NLBENCH), str(path), str(runs + 1)]), capture_output=True, text=True,
                               env={**os.environ, **RETAINING_MALLOC})
            rec["read_ms"][f"nanolance-cpp-retain <- {file_label}"] = json.loads(r.stdout)["warm_median_ms"]
        except Exception as e:  # noqa: BLE001
            rec["errors"][f"read {key}"] = str(e)[:300]
        key = f"nanolance-cpp-mt <- {file_label}"
        try:
            out = native([str(NLBENCH), str(path), str(runs + 1)])
            if out["rows"] != table.num_rows:
                raise RuntimeError(f"read {out['rows']} rows, expected {table.num_rows}")
            rec["read_ms"][key] = out["warm_median_ms"]
            rec["peak_mb"][f"nanolance-cpp-mt read <- {file_label}"] = out["peak_rss_mb"]
        except Exception as e:  # noqa: BLE001
            rec["errors"][f"read {key}"] = str(e)[:300]
        if RUST_BENCH.exists():
            for label, one_core in (("rust-native", False), ("rust-native-1c", True)):
                key = f"{label} <- {file_label}"
                dump = work / f"{name}_rsn_dump.arrow"
                try:
                    out = native([str(RUST_BENCH), "read", str(path), str(runs + 1), "--dump", str(dump)],
                                 one_core=one_core)
                    got = ipc.open_stream(dump).read_all()
                    if not same(got, table):
                        rec["errors"][f"read {key}"] = "returned different data"
                        continue
                    rec["read_ms"][key] = out["warm_median_ms"]
                    rec["peak_mb"][f"{label} read <- {file_label}"] = out["peak_rss_mb"]
                except Exception as e:  # noqa: BLE001
                    rec["errors"][f"read {key}"] = str(e)[:300]
                finally:
                    dump.unlink(missing_ok=True)
        key = f"rust-lance-1c <- {file_label}"
        if f"read rust-lance <- {file_label}" not in rec["errors"]:
            try:
                rec["read_ms"][key] = rust_one_core("read", path, runs)
            except Exception as e:  # noqa: BLE001
                rec["errors"][f"read {key}"] = str(e)[:300]
    try:
        if not same(pq.read_table(pq_path), table):
            rec["errors"]["read parquet"] = "returned different data"
        else:
            rec["read_ms"]["parquet <- parquet"] = timed(lambda: pq.read_table(pq_path), runs)
    except Exception as e:  # noqa: BLE001
        rec["errors"]["read parquet"] = str(e)[:300]

    for p in (ipc_path, nl_path, py_path, rust_path, pq_path):
        if p.is_dir():
            shutil.rmtree(p, ignore_errors=True)
        elif p.exists():
            p.unlink()
    if verbose:
        w = rec["write_ms"]
        r = rec["read_ms"]
        print(f"  {name:20s} write nl-cpp {w.get('nanolance-cpp', float('nan')):8.1f} rust {w.get('rust-lance', float('nan')):8.1f}"
              f" | read nl-cpp<-nl {r.get('nanolance-cpp <- nanolance', float('nan')):7.1f}"
              f" rust<-rust {r.get('rust-lance <- rust-lance', float('nan')):7.1f}"
              f" rust1c<-rust {r.get('rust-lance-1c <- rust-lance', float('nan')):7.1f}"
              f" rsn<-rust {r.get('rust-native <- rust-lance', float('nan')):7.1f}"
              f" rsn1c<-rust {r.get('rust-native-1c <- rust-lance', float('nan')):7.1f}"
              f" | size nl {rec['size'].get('nanolance', 0) / 1e6:6.1f}MB rust {rec['size'].get('rust-lance', 0) / 1e6:6.1f}MB"
              + (f" | ERR {list(rec['errors'])}" if rec["errors"] else ""), flush=True)
    return rec


def footprint():
    """What each side costs to ship: stripped binary sizes, the Python packages' native libraries,
    and the third-party code linked in."""

    def stripped_size(path):
        path = Path(path)
        if not path.exists():
            return None
        if not shutil.which("strip"):
            return path.stat().st_size
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / path.name
            shutil.copy(path, copy)
            subprocess.run(["strip", str(copy)], capture_output=True)
            return copy.stat().st_size

    def native_lib(module):
        """The largest native library the package has loaded (its extension module)."""
        files = {Path(m.__file__) for name, m in list(sys.modules.items())
                 if (name == module.__name__ or name.startswith(module.__name__ + "."))
                 and getattr(m, "__file__", None) and str(m.__file__).endswith(".so")}
        return max((f.stat().st_size for f in files), default=None)

    crates = None
    if RUST_BENCH.exists() and shutil.which("cargo"):
        r = subprocess.run(["cargo", "tree", "-e", "normal", "--prefix", "none"], cwd=RUST_BENCH.parents[2],
                           capture_output=True, text=True)
        if r.returncode == 0:
            crates = len({line.replace(" (*)", "").strip() for line in r.stdout.splitlines() if line.strip()}) - 1
    return {
        "nanolance": {
            "bench_binary_bytes": stripped_size(NLBENCH),
            "python_native_lib_bytes": native_lib(nanolance),
            "third_party": ["nanoarrow (its IPC reader bundles flatcc)", "zstd"],
            "binary": "tools/nlbench: reader + writer + Arrow IPC input, static, stripped",
        },
        "rust": {
            "bench_binary_bytes": stripped_size(RUST_BENCH),
            "python_native_lib_bytes": native_lib(lance),
            "crates": crates,
            "binary": "tools/lance_rs_bench: lance 12.0.0, default features off (local files only), thin LTO, stripped",
        },
    }


def environment():
    def git(*args):
        try:
            return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()
        except OSError:
            return ""
    cpu = ""
    try:
        cpu = next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")), "")
    except OSError:
        pass
    mem_gb = 0
    try:
        mem_gb = round(int(next(l for l in open("/proc/meminfo") if l.startswith("MemTotal")).split()[1]) / 2**20)
    except (OSError, StopIteration):
        pass
    return {
        "date": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "cpu": cpu or platform.processor(),
        "cores": os.cpu_count(),
        "memory_gb": mem_gb,
        "os": f"{platform.system()} {platform.release()}",
        "python": platform.python_version(),
        "pylance": lance.__version__,
        "pyarrow": pa.__version__,
        "nanolance_commit": git("rev-parse", "--short", "HEAD"),
        "build": "Release" if "Release" in (BUILD / "CMakeCache.txt").read_text(errors="ignore") else "see CMakeCache",
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--quick", action="store_true", help="a tenth of the rows")
    ap.add_argument("--only", nargs="*", help="dataset names to run")
    ap.add_argument("--out", default=str(ROOT / "bench" / "results" / "matrix.json"))
    ap.add_argument("--worker", nargs="+", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    if args.worker:
        action, path, *rest = args.worker
        worker(action, path, args.runs, rest[0] if rest else None)
        return 0
    scale = 0.1 if args.quick else 1.0

    datasets = build_datasets(scale)
    names = args.only or list(datasets) + ["mixed_events"]
    results = {"environment": environment(), "footprint": footprint(), "runs": args.runs, "scale": scale,
               "datasets": {}}
    work = Path(tempfile.mkdtemp(prefix="nlmatrix-"))
    print(f"environment: {results['environment']}", flush=True)
    try:
        for name in names:
            if name == "mixed_events":
                table, category, description = mixed_table(scale), "mixed", "10-column event table (ints, timestamps, strings, list, struct, nulls)"
            else:
                category, description, rows, build = datasets[name]
                table = pa.table({"c": build(rows)})
            rec = run_one(name, table, args.runs, work, verbose=True)
            rec.update(category=category, description=description)
            results["datasets"][name] = rec
    finally:
        shutil.rmtree(work, ignore_errors=True)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(results, indent=1))
    print(f"wrote {args.out}")


if __name__ == "__main__":
    sys.exit(main())
