#!/usr/bin/env python3
"""Data overlays against update(), and the read cost of stacked overlays: nanolance and pylance 12.

    python tools/bench_overlay.py [--rows 1000000] [--repeat 3] [--dir /tmp/bench_overlay]

Patch: new values for a fraction of the rows of one int64 column of a one-fragment dataset, either as a DataOverlay
(a small data file plus a commit; the fragment's files stay) or as update() (pylance rewrites the rows it changes).
Read: a full scan, a one-column scan and a take of 1,000 random rows, with 0, 1 and 10 overlays stacked on the
fragment. Times are the best of --repeat; sizes are the bytes the patch added to the dataset directory.
"""

from __future__ import annotations

import argparse
import os
import shutil
import time

import numpy as np
import pyarrow as pa

os.environ.setdefault("LANCE_ENABLE_UNSTABLE_DATA_OVERLAY_FILES", "1")  # pylance 12 opens overlays only with this

import lance  # noqa: E402

import nanolance.lance as nl  # noqa: E402

ENGINES = {"nanolance": nl, "pylance": lance}


def table(rows: int) -> pa.Table:
    rng = np.random.default_rng(0)
    return pa.table({"id": pa.array(np.arange(rows, dtype=np.int64)),
                     "val": pa.array(rng.integers(0, 1 << 40, rows)),
                     "s": pa.array([f"name-{i:010d}" for i in range(rows)])})


def dir_bytes(path: str) -> int:
    return sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(path) for f in fs)


def overlay(m, path: str, offsets: np.ndarray, values: np.ndarray, name: str):
    ds = m.dataset(path)
    with m.file.LanceFileWriter(os.path.join(path, "data", name)) as w:
        w.write_batch(pa.table({"val": pa.array(values)}))
    base = ds.get_fragments()[0].metadata.files[0]
    data_file = m.fragment.DataFile(path=name, fields=[1], column_indices=[0],
                                    file_major_version=base.file_major_version,
                                    file_minor_version=base.file_minor_version,
                                    file_size_bytes=os.path.getsize(os.path.join(path, "data", name)))
    op = m.LanceOperation.DataOverlay([m.LanceOperation.DataOverlayGroup(
        ds.get_fragments()[0].fragment_id, [m.LanceOperation.DataOverlayFile(data_file, offsets=offsets.tolist())])])
    m.LanceDataset.commit(path, op, read_version=ds.version)


def best(fn, repeat: int) -> float:
    out = float("inf")
    for _ in range(repeat):
        start = time.perf_counter()
        fn()
        out = min(out, time.perf_counter() - start)
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=1_000_000)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--dir", default="/tmp/bench_overlay")
    args = ap.parse_args()
    shutil.rmtree(args.dir, ignore_errors=True)
    os.makedirs(args.dir)
    base = os.path.join(args.dir, "base.lance")
    t = table(args.rows)
    nl.write_dataset(t, base)
    base_bytes = dir_bytes(base)
    rng = np.random.default_rng(1)

    print(f"## Patch {args.rows:,} rows, one int64 column (best of {args.repeat})\n")
    print("| rows patched | engine | overlay | update() | bytes added: overlay / update |")
    print("|---|---|---|---|---|")
    for fraction in (0.001, 0.01, 0.1):
        k = max(1, int(args.rows * fraction))
        offsets = np.sort(rng.choice(args.rows, k, replace=False))
        values = rng.integers(0, 1 << 40, k)
        step = int(round(1 / fraction))
        for name, m in ENGINES.items():
            sizes, times = {}, {}
            for how in ("overlay", "update"):
                def run():
                    p = os.path.join(args.dir, f"{name}_{how}.lance")
                    shutil.rmtree(p, ignore_errors=True)
                    shutil.copytree(base, p)
                    start = time.perf_counter()
                    if how == "overlay":
                        overlay(m, p, offsets, values, "patch.lance")
                    else:
                        m.dataset(p).update({"val": "val + 1"}, where=f"id % {step} = 0")
                    times.setdefault(how, []).append(time.perf_counter() - start)
                    sizes[how] = dir_bytes(p) - base_bytes
                for _ in range(args.repeat):
                    run()
            print(f"| {k:,} ({fraction:.1%}) | {name} | {min(times['overlay']) * 1000:.1f} ms | "
                  f"{min(times['update']) * 1000:.1f} ms | {sizes['overlay']:,} / {sizes['update']:,} |")

    print(f"\n## Read with overlays stacked (best of {args.repeat}; 1% of rows patched by each overlay)\n")
    print("| overlays | engine | full scan | `val` scan | take 1,000 rows |")
    print("|---|---|---|---|---|")
    picks = np.sort(rng.choice(args.rows, 1000, replace=False)).tolist()
    for stacked in (0, 1, 10):
        path = os.path.join(args.dir, f"stacked_{stacked}.lance")
        shutil.copytree(base, path)
        for i in range(stacked):
            offsets = np.sort(rng.choice(args.rows, args.rows // 100, replace=False))
            overlay(nl, path, offsets, rng.integers(0, 1 << 40, len(offsets)), f"patch{i}.lance")
        expected = nl.dataset(path).to_table(columns=["val"])
        assert lance.dataset(path).to_table(columns=["val"]).equals(expected), "engines disagree"
        for name, m in ENGINES.items():
            full = best(lambda: m.dataset(path).to_table(), args.repeat)
            col = best(lambda: m.dataset(path).to_table(columns=["val"]), args.repeat)
            take = best(lambda: m.dataset(path).take(picks), args.repeat)
            print(f"| {stacked} | {name} | {full * 1000:.1f} ms | {col * 1000:.1f} ms | {take * 1000:.1f} ms |")


if __name__ == "__main__":
    main()
