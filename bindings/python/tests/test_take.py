"""Random access: nanolance.take / lance_table_take / nano_lance_table_take.

A training loader reads shuffled mini-batches, not the whole table. take() has to return exactly
what pylance's LanceDataset.take returns -- the rows at those indices, in that order, repeats
included -- for every column type, on files either writer made, across fragments and deletions.
Each fragment and page without a requested row is skipped; large values (FullZip pages) are read
row by row through the page's per-row index.
"""

from __future__ import annotations

import os
import subprocess
import sys

import numpy as np
import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance

N = 20_000


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _table(n=N, seed=0):
    rng = np.random.default_rng(seed)
    valid = rng.random(n) > 0.1
    return pa.table({
        "id": pa.array(np.arange(n, dtype=np.int64)),
        "small": pa.array(rng.integers(0, 100, n, dtype=np.int32)),
        "f": pa.array(rng.random(n), mask=~valid),
        "flag": pa.array(rng.random(n) < 0.3),
        "tag": pa.array([f"tag{i % 7}" for i in range(n)]),
        "text": pa.array([None if i % 17 == 0 else f"user-{rng.integers(1 << 30)}@example.org" for i in range(n)]),
        "blob": pa.array([rng.bytes(int(s)) for s in rng.integers(0, 64, n)], pa.binary()),
        "vec": pa.FixedSizeListArray.from_arrays(pa.array(rng.random(n * 4, dtype=np.float32)), 4),
        "items": pa.array([[int(j) for j in range(i % 5)] if i % 11 else None for i in range(n)], pa.list_(pa.int64())),
        "point": pa.array([{"x": i, "y": float(i) / 2} for i in range(n)]),
        "attrs": pa.array([[("k", i)] for i in range(n)], pa.map_(pa.utf8(), pa.int64())),
    })


def _images(n=400, seed=1):
    rng = np.random.default_rng(seed)
    return pa.table({
        "id": pa.array(range(n), pa.int64()),
        "image": pa.array([None if i % 50 == 7 else rng.bytes(int(s)) for i, s in
                           enumerate(rng.integers(5_000, 120_000, n))], pa.binary()),
        "label": pa.array([f"class-{i % 13}" for i in range(n)]),
    })


def _write(kind, table, path, lance_mod, **kwargs):
    if kind == "nanolance":
        nanolance.write_table(table, path, **kwargs)
    else:
        lance_mod.write_dataset(table, str(path), data_storage_version="2.2")


INDEX_SETS = {
    "one": [12_345],
    "sorted": list(range(0, N, 997)),
    "shuffled": [N - 1, 3, 17_000, 3, 0, 9_999, 10_000, 17_000],
    "dense run": list(range(4_000, 4_300)),
    "empty": [],
}


@pytest.mark.parametrize("writer", ["nanolance", "rust"])
@pytest.mark.parametrize("which", list(INDEX_SETS))
def test_take_matches_pylance(lance_mod, tmp_path, writer, which):
    table = _table()
    path = tmp_path / "t.lance"
    _write(writer, table, path, lance_mod)
    idx = INDEX_SETS[which]
    got = pa.table(nanolance.take(path, idx))
    assert got.to_pydict() == table.take(pa.array(idx, pa.int64())).to_pydict()
    if idx:
        assert got.to_pydict() == lance_mod.dataset(str(path)).take(idx).to_pydict()


@pytest.mark.parametrize("writer", ["nanolance", "rust"])
def test_take_images(lance_mod, tmp_path, writer):
    table = _images()
    path = tmp_path / "images.lance"
    _write(writer, table, path, lance_mod)
    rng = np.random.default_rng(2)
    for _ in range(5):
        idx = rng.choice(table.num_rows, 64, replace=False).tolist()
        got = pa.table(nanolance.take(path, idx, columns=["image", "label"]))
        assert got.to_pydict() == table.select(["image", "label"]).take(pa.array(idx)).to_pydict()


def test_take_across_fragments_and_deletions(lance_mod, tmp_path):
    table = _table()
    path = tmp_path / "frag.lance"
    with nanolance.LanceWriter(path, max_rows_per_fragment=3_000) as writer:
        for batch in table.to_batches(max_chunksize=1_000):
            writer.write_batch(batch)
    ds = lance_mod.dataset(str(path))
    assert len(ds.get_fragments()) > 5
    ds.delete("id % 7 = 3 OR (id >= 6000 AND id < 6500)")
    remaining = lance_mod.dataset(str(path)).to_table()
    rng = np.random.default_rng(3)
    idx = rng.choice(remaining.num_rows, 300, replace=True).tolist()
    got = pa.table(nanolance.take(path, idx))
    assert got.to_pydict() == remaining.take(pa.array(idx)).to_pydict()
    assert got.to_pydict() == lance_mod.dataset(str(path)).take(idx).to_pydict()


def test_take_projection_and_errors(tmp_path):
    table = _table(2_000)
    path = tmp_path / "p.lance"
    nanolance.write_table(table, path)
    got = pa.table(nanolance.take(path, [5, 1], columns=["text", "point"]))
    assert got.column_names == ["text", "point"]
    assert got.to_pydict() == table.select(["text", "point"]).take(pa.array([5, 1])).to_pydict()
    with pytest.raises(IndexError):
        nanolance.take(path, [2_000])
    with pytest.raises(IndexError):
        nanolance.take(path, [-1])
    with pytest.raises(Exception):
        nanolance.take(path, [0], columns=["nope"])


def test_full_read_of_a_rust_map_with_a_constant_key(lance_mod, tmp_path):
    """pylance writes a map whose keys are all the same as a Constant page with repetition levels and
    an empty definition buffer (every layer all-valid); the reader required definition levels there."""
    table = pa.table({"attrs": pa.array([[("k", i)] for i in range(5_000)], pa.map_(pa.utf8(), pa.int64()))})
    path = tmp_path / "map.lance"
    lance_mod.write_dataset(table, str(path), data_storage_version="2.2")
    assert pa.table(nanolance.read_table(path)).to_pydict() == table.to_pydict()


def test_take_without_the_decoded_column_cache(tmp_path):
    """Small columns are decoded once and kept for later takes (NANOLANCE_TAKE_CACHE_MB, default
    256); with the cache off every take decodes just the pages it touches. Both give the same rows."""
    table = _table(5_000)
    path = tmp_path / "c.lance"
    nanolance.write_table(table, path)
    idx = [4_999, 0, 77, 77, 2_500, 1_023, 1_024]
    script = (
        "import sys, pyarrow as pa, nanolance\n"
        f"t = pa.table(nanolance.take({str(path)!r}, {idx!r}))\n"
        "sys.stdout.write(repr(t.to_pydict()))\n"
    )
    expected = repr(table.take(pa.array(idx, pa.int64())).to_pydict())
    for budget in ("0", "256"):
        env = {**os.environ, "NANOLANCE_TAKE_CACHE_MB": budget}
        out = subprocess.run([sys.executable, "-c", script], env=env, check=True, capture_output=True, text=True)
        assert out.stdout == expected, f"NANOLANCE_TAKE_CACHE_MB={budget}"
