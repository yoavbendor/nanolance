"""Data overlay files (Lance's DataOverlay): new values for some cells of a fragment without rewriting
its data files. Every case runs on a nanolance and a pylance dataset (or commits with one engine and
reads with the other) and compares what each reads back.
"""

from __future__ import annotations

import os

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


@pytest.fixture(autouse=True)
def overlays_enabled(monkeypatch):
    # pylance 12 opens a dataset with overlays (feature flag 64) only with this set.
    monkeypatch.setenv("LANCE_ENABLE_UNSTABLE_DATA_OVERLAY_FILES", "1")


def table(n=10):
    return pa.table({"id": pa.array(range(n), pa.int32()),
                     "val": pa.array([i * 10 for i in range(n)], pa.int32()),
                     "s": pa.array([f"s{i}" for i in range(n)])})


def overlay(m, ds, path, name, values: pa.Table, fields, offsets):
    """Write `values` as an overlay file with engine `m` and commit it over the first fragment at `offsets`."""
    with m.file.LanceFileWriter(os.path.join(path, "data", name)) as w:
        w.write_batch(values)
    base = ds.get_fragments()[0].metadata.files[0]
    df = m.fragment.DataFile(path=name, fields=fields, column_indices=list(range(len(fields))),
                             file_major_version=base.file_major_version, file_minor_version=base.file_minor_version,
                             file_size_bytes=os.path.getsize(os.path.join(path, "data", name)))
    op = m.LanceOperation.DataOverlay([m.LanceOperation.DataOverlayGroup(
        ds.get_fragments()[0].fragment_id, [m.LanceOperation.DataOverlayFile(df, offsets=offsets)])])
    return m.LanceDataset.commit(path, op, read_version=ds.version)


def both(lance, tmp_path, steps):
    out = []
    for name, m in (("nano", nl), ("py", lance)):
        path = str(tmp_path / name)
        ds = m.write_dataset(table(), path)
        out.append(steps(m, ds, path))
    return out


def test_dense_sparse_and_newest_wins(lance, tmp_path):
    def steps(m, ds, path):
        ds = overlay(m, ds, path, "a.lance", pa.table({"val": pa.array([111, 444], pa.int32())}), [1], [4, 1])
        ds = overlay(m, ds, path, "b.lance", pa.table({"val": pa.array([999], pa.int32())}), [1], [1])
        ds = overlay(m, ds, path, "c.lance", pa.table({"id": pa.array([77], pa.int32()),
                                                        "val": pa.array([33], pa.int32())}), [0, 1], [[2], [3]])
        return ds.to_table().to_pydict()

    nano, py = both(lance, tmp_path, steps)
    assert nano == py
    assert nano["val"][:5] == [0, 999, 20, 33, 444]


def test_strings_nulls_deletions_and_take(lance, tmp_path):
    def steps(m, ds, path):
        ds = overlay(m, ds, path, "a.lance", pa.table({"s": pa.array(["one", None, "five"])}), [2], [1, 3, 5])
        ds.delete("id = 5")
        return (ds.to_table().to_pydict(), ds.take([0, 1, 2, 3], columns=["s"]).to_pydict(),
                ds.to_table(columns=["id"], filter="s = 'one'").to_pydict())

    nano, py = both(lance, tmp_path, steps)
    assert nano == py
    assert nano[0]["s"][:4] == ["s0", "one", "s2", None]


def test_stale_scalar_index_is_not_trusted(lance, tmp_path):
    def steps(m, ds, path):
        ds.create_scalar_index("val", "BTREE")
        ds = overlay(m, ds, path, "a.lance", pa.table({"val": pa.array([999], pa.int32())}), [1], [1])
        return (ds.to_table(columns=["id"], filter="val = 999").to_pydict(),
                ds.to_table(columns=["id"], filter="val = 10").to_pydict())

    nano, py = both(lance, tmp_path, steps)
    assert nano == py == ({"id": [1]}, {"id": []})


def test_stale_vector_index_is_not_trusted(lance, tmp_path):
    rng = np.random.default_rng(3)
    vectors = rng.standard_normal((300, 8)).astype(np.float32)

    def steps(m, ds, path):
        ds = m.write_dataset(pa.table({"id": pa.array(range(300), pa.int64()),
                                       "v": pa.FixedSizeListArray.from_arrays(pa.array(vectors.ravel()), 8)}),
                             path, mode="overwrite")
        ds.create_index("v", "IVF_FLAT", num_partitions=2, metric="l2")
        moved = pa.FixedSizeListArray.from_arrays(pa.array(np.zeros(16, np.float32)), 8)
        ds = overlay(m, ds, path, "v.lance", pa.table({"v": moved}), [1], [3, 7])
        got = ds.to_table(columns=["id"], nearest={"column": "v", "q": np.zeros(8, np.float32), "k": 2})
        return sorted(got["id"].to_pylist())

    nano, py = both(lance, tmp_path, steps)
    assert nano == py == [3, 7]


def test_each_engine_reads_the_other_engines_overlays(lance, tmp_path):
    for writer, reader in ((nl, lance), (lance, nl)):
        path = str(tmp_path / f"x_{writer.__name__}")
        ds = writer.write_dataset(table(), path)
        ds = overlay(writer, ds, path, "a.lance", pa.table({"val": pa.array([5, 6], pa.int32())}), [1], [0, 9])
        other = reader.dataset(path)
        assert other.to_table().to_pydict() == ds.to_table().to_pydict()
        meta = other.get_fragments()[0].metadata.overlays[0]
        assert list(meta.offsets) == [0, 9] and meta.committed_version == ds.version


def test_compaction_keeps_the_overlaid_values(lance, tmp_path):
    path = str(tmp_path / "c")
    ds = nl.write_dataset(table(), path, max_rows_per_file=4)
    ds = overlay(nl, ds, path, "a.lance", pa.table({"val": pa.array([-1], pa.int32())}), [1], [2])
    before = ds.to_table().to_pydict()
    ds.optimize.compact_files(target_rows_per_fragment=100)
    assert ds.to_table().to_pydict() == before == lance.dataset(path).to_table().to_pydict()


def test_example_runs(lance, tmp_path):
    """examples/data_overlay.py: patches three cells with an overlay and both engines read them back."""
    import importlib.util
    from pathlib import Path

    spec = importlib.util.spec_from_file_location(
        "data_overlay_example", Path(__file__).resolve().parents[1] / "examples" / "data_overlay.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    path = str(tmp_path / "demo.lance")
    module.main(path)
    assert lance.dataset(path).to_table().column("price").to_pylist() == [0, 10, 999, 30, 40, 555, 60, 777, 80, 90]
