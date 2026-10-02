"""The real training datasets, when they are on disk: COCO 2017 val and Speech Commands v0.02.

Skipped unless NANOLANCE_DATASETS names the directory tools/bench_multimodal.py reads (its header
lists the files and where they come from; they are not redistributed here). NANOLANCE_DATASETS_ROWS
caps the rows used (default 1,000; 0 for all).

These are the files that found the bugs and slowdowns the rest of the suite now pins in synthetic
form (tests/test_training_shapes.py, tests/test_work_guards.py). Real data keeps finding what
synthetic data does not, so the same checks run on it: both writers, every read checked against the
source table and against pylance, one thread and four, and the work guards on the real shapes.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import numpy as np
import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance

DATA = os.environ.get("NANOLANCE_DATASETS")
pytestmark = pytest.mark.skipif(not DATA, reason="NANOLANCE_DATASETS is not set (see tools/bench_multimodal.py)")

ROOT = Path(__file__).resolve().parents[3]


def _builders():
    sys.path.insert(0, str(ROOT / "tools"))
    try:
        import bench_multimodal
    finally:
        sys.path.pop(0)
    return bench_multimodal


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


@pytest.fixture(scope="module", params=["coco", "speech"])
def dataset(request, lance_mod, tmp_path_factory):
    bm = _builders()
    spec = bm.DATASETS[request.param]
    table = spec["build"](Path(DATA))
    rows = int(os.environ.get("NANOLANCE_DATASETS_ROWS", "1000"))
    if rows:
        table = table.slice(0, min(rows, table.num_rows))
    table = pa.Table.from_batches(table.combine_chunks().to_batches())
    work = tmp_path_factory.mktemp(request.param)
    paths = {"nanolance": work / "nl.lance", "rust": work / "rust.lance"}
    nanolance.write_table(table, paths["nanolance"])
    lance_mod.write_dataset(table, str(paths["rust"]), data_storage_version="2.2")
    return request.param, table, spec, paths


@pytest.fixture(params=[1, 4], ids=["1 thread", "4 threads"])
def n_threads(request):
    before = nanolance.get_threads()
    nanolance.set_threads(request.param)
    yield request.param
    nanolance.set_threads(before)


@pytest.mark.parametrize("writer", ["nanolance", "rust"])
def test_every_read(lance_mod, dataset, writer, n_threads):
    name, table, spec, paths = dataset
    path = paths[writer]
    assert pa.table(nanolance.read_table(path)).to_pydict() == table.to_pydict()
    meta = [c for c in table.column_names if c not in spec["heavy"]]
    assert pa.table(nanolance.read_table(path, columns=meta)).to_pydict() == table.select(meta).to_pydict()
    if writer == "nanolance":
        assert lance_mod.dataset(str(path)).to_table().to_pydict() == table.to_pydict()


@pytest.mark.parametrize("writer", ["nanolance", "rust"])
def test_a_shuffled_epoch(lance_mod, dataset, writer, n_threads):
    name, table, spec, paths = dataset
    cols = spec["train"]
    order = np.random.default_rng(0).permutation(table.num_rows).tolist()
    ds = lance_mod.dataset(str(paths[writer]))
    for start in range(0, len(order), 64):
        batch = order[start:start + 64]
        got = pa.table(nanolance.take(paths[writer], batch, columns=cols))
        assert got.to_pydict() == table.select(cols).take(pa.array(batch)).to_pydict()
        if start % 640 == 0:
            assert got.to_pydict() == ds.take(batch, columns=cols).to_pydict()


def test_take_reads_about_what_it_returns(dataset):
    """The guard of test_work_guards.py on the real files: a mini-batch of images or clips reads a
    small multiple of their size, whichever writer made the file."""
    name, table, spec, paths = dataset
    heavy = spec["heavy"][0]
    rows = np.random.default_rng(1).choice(table.num_rows, 64, replace=False).tolist()
    column = table.column(heavy).take(pa.array(rows))
    wanted = column.nbytes
    for writer, path in paths.items():
        nanolance.take(path, rows[:1], columns=[heavy])
        nanolance._reset_work_stats()
        got = pa.table(nanolance.take(path, rows, columns=[heavy]))
        stats = nanolance._work_stats()
        assert got.column(heavy).to_pylist() == column.to_pylist()
        assert stats["data_bytes_read"] < 4 * wanted + (1 << 20), (writer, stats, wanted)
