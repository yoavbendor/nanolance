"""Memory safety smoke tests for the bindings."""

from __future__ import annotations

import gc

import pyarrow as pa
import pytest

import nanolance

psutil = pytest.importorskip("psutil")
PROC = psutil.Process()


def _rss_mb() -> float:
    return PROC.memory_info().rss / (1024 * 1024)


def test_lance_repeated_roundtrip_bounded(sample_table, tmp_path):
    baseline = _rss_mb()
    for i in range(20):
        ds = tmp_path / f"mem_{i}.lance"
        nanolance.write_table(sample_table, ds, compression=True)
        _ = pa.table(nanolance.read_table(ds))
    gc.collect()
    growth = _rss_mb() - baseline
    assert growth < 250, f"RSS grew by {growth:.1f} MB after 20 lance cycles"


def test_large_single_batch_write(tmp_path):
    n = 1_000_000
    table = pa.table({"x": pa.array(range(n), type=pa.int64())})
    path = tmp_path / "big.lance"
    before = _rss_mb()
    nanolance.write_table(table, path, compression=True)
    gc.collect()
    after = _rss_mb()
    assert after - before < 300


@pytest.mark.parametrize(
    "corrupt",
    [
        pytest.param(lambda p: (p / "_versions").rename(p / "_gone"), id="manifest_missing"),
        pytest.param(
            lambda p: next((p / "data").glob("*.lance")).write_bytes(b"not a lance file"),
            id="data_file_garbage",
        ),
        pytest.param(
            lambda p: next((p / "data").glob("*.lance")).write_bytes(b""),
            id="data_file_empty",
        ),
    ],
)
def test_failed_read_raises_instead_of_crashing(corrupt, tmp_path):
    """A read that fails must raise, not take the interpreter down with it.

    lance_table_read_dataset releases out_schema on its mid-read failure path; the C shim released it
    a second time. ArrowSchemaRelease dereferences `release` unconditionally and releasing nulls it,
    so the second call jumped through a null pointer -- EVERY failed read through the C API or these
    bindings segfaulted rather than reporting its error. A library that crashes the process on a bad
    file is worse than one that rejects it, and it undercut the whole hardened-reader posture.
    """
    path = tmp_path / "victim.lance"
    nanolance.write_table(pa.table({"a": pa.array([1, 2, 3], type=pa.int64())}), path)
    corrupt(path)
    with pytest.raises(RuntimeError):
        nanolance.read_table(path)
    # Still here, and the process is still healthy enough to do real work.
    ok = tmp_path / "after.lance"
    nanolance.write_table(pa.table({"a": pa.array([4, 5], type=pa.int64())}), ok)
    assert pa.table(nanolance.read_table(ok)).column(0).to_pylist() == [4, 5]
