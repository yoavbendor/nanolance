"""Performance and output-size benchmarks (informational; soft gates)."""

from __future__ import annotations

import os
import random
import statistics
import time

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


def _median_seconds(fn, *, iters: int = 3) -> float:
    return statistics.median(fn() for _ in range(iters))


def _pylance_write_seconds(lance, table: pa.Table, path, *, rayon_threads: int | None) -> float:
    old = os.environ.get("RAYON_NUM_THREADS")
    if rayon_threads is not None:
        os.environ["RAYON_NUM_THREADS"] = str(rayon_threads)
    try:
        return _median_seconds(
            lambda: _write_once(lance.write_dataset, table, path),
        )
    finally:
        if old is None:
            os.environ.pop("RAYON_NUM_THREADS", None)
        else:
            os.environ["RAYON_NUM_THREADS"] = old


def _write_once(write_fn, table: pa.Table, path) -> float:
    if os.path.exists(path):
        if os.path.isdir(path):
            import shutil

            shutil.rmtree(path)
        else:
            os.remove(path)
    t0 = time.perf_counter()
    write_fn(table, path)
    return time.perf_counter() - t0


def _wide_int_table(n: int) -> pa.Table:
    random.seed(1)
    return pa.table(
        {
            "ts": pa.array(
                [1_700_000_000_000_000 + i * 1500 + random.randint(0, 200) for i in range(n)],
                pa.uint64(),
            ),
            "caplen": pa.array([random.randint(60, 1514) for _ in range(n)], pa.uint32()),
            "iface": pa.array([(i // 10_000) % 4 for i in range(n)], pa.uint8()),
            "ipproto": pa.array([random.choice([6, 17, 6, 6, 1]) for _ in range(n)], pa.uint8()),
        }
    )


def _pcap_ref_table(n: int) -> pa.Table:
    min_distinct = n // 5000
    return pa.table(
        {
            "uri": pa.array(
                [f"s3://bucket/cap_{(i * min_distinct // n):02d}.pcapng" for i in range(n)],
                pa.string(),
            ),
            "position": pa.array([i * 1500 for i in range(n)], pa.uint64()),
            "size": pa.array([1500] * n, pa.uint64()),
        }
    )


@pytest.mark.bench
def test_lance_write_wide_int_near_pylance(tmp_path):
    """Integer-heavy shape: nanolance C++ bench is ~1.3× pylance on this profile."""
    lance = require_pylance()
    n = 200_000
    table = _wide_int_table(n)

    nl_path = tmp_path / "nanolance.lance"
    nl_s = _median_seconds(
        lambda: _write_once(
            lambda t, p: nanolance.write_table(t, p, compression=False),
            table,
            nl_path,
        ),
    )

    pl_path = tmp_path / "pylance.lance"
    pl_s = _pylance_write_seconds(lance, table, pl_path, rayon_threads=1)

    assert pa.table(nanolance.read_table(nl_path)).num_rows == n

    ratio = nl_s / max(pl_s, 1e-9)
    print(f"lance wide_int write: nanolance={nl_s:.3f}s pylance(1t)={pl_s:.3f}s ratio={ratio:.2f}x")
    assert ratio < 4.0


@pytest.mark.bench
def test_lance_write_pcap_ref_with_zstd_near_pylance(tmp_path):
    """Repetitive URI runs: nanolance dict-RLE + zstd matches or beats pylance."""
    lance = require_pylance()
    n = 200_000
    table = _pcap_ref_table(n)

    nl_path = tmp_path / "nanolance.lance"
    nl_s = _median_seconds(
        lambda: _write_once(
            lambda t, p: nanolance.write_table(t, p, compression=True),
            table,
            nl_path,
        ),
    )

    pl_path = tmp_path / "pylance.lance"
    pl_s = _pylance_write_seconds(lance, table, pl_path, rayon_threads=1)

    assert pa.table(nanolance.read_table(nl_path)).num_rows == n

    ratio = nl_s / max(pl_s, 1e-9)
    print(f"lance pcap_ref+zstd write: nanolance={nl_s:.3f}s pylance(1t)={pl_s:.3f}s ratio={ratio:.2f}x")
    assert ratio < 3.0


@pytest.mark.bench
def test_lance_scattered_strings_near_pylance(tmp_path):
    """Cycling low-card strings: structural dictionary encoding should be near pylance parity."""
    lance = require_pylance()
    n = 200_000
    table = pa.table(
        {
            "id": pa.array(range(n), type=pa.int64()),
            "tag": pa.array([f"row-{i % 500}" for i in range(n)], type=pa.string()),
            "value": pa.array([float(i) * 0.01 for i in range(n)], type=pa.float64()),
        }
    )

    nl_path = tmp_path / "nanolance.lance"
    nl_s = _median_seconds(
        lambda: _write_once(
            lambda t, p: nanolance.write_table(t, p, compression=True),
            table,
            nl_path,
        ),
    )

    pl_path = tmp_path / "pylance.lance"
    pl_1t = _pylance_write_seconds(lance, table, pl_path, rayon_threads=1)

    ratio_1t = nl_s / max(pl_1t, 1e-9)
    print(f"lance scattered-string write: nanolance={nl_s:.3f}s pylance(1t)={pl_1t:.3f}s ratio={ratio_1t:.2f}x")
    assert ratio_1t < 4.0
