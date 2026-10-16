"""Stable row ids: nanolance.lance against pylance on the same operations, and across the two engines.

A dataset written with ``enable_stable_row_ids=True`` gives each row an id that survives update,
merge_insert, compaction and restore (``_rowid`` != ``_rowaddr``). Every case runs the same steps on a
nanolance dataset and a pylance dataset and compares what each reads back -- rows by key, their
``_rowid`` and version columns, the bytes of each fragment's row id sequence -- and opens the
nanolance-written dataset with pylance.
"""

from __future__ import annotations

import pickle

import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance.fragment import RowDatasetVersionMeta, RowIdMeta, RowIdSequence
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def table(n=10, start=0):
    return pa.table({"id": pa.array(range(start, start + n), pa.int64()),
                     "v": pa.array([i * 10 for i in range(start, start + n)], pa.int64())})


def state(ds, *, with_versions=True):
    cols = ["id", "v"]
    t = ds.to_table(columns=cols, with_row_id=True)
    out = {"rows": sorted(zip(t["id"].to_pylist(), t["v"].to_pylist(), t["_rowid"].to_pylist()))}
    if with_versions:
        t = ds.to_table(columns=["id", "_row_created_at_version", "_row_last_updated_at_version"])
        out["versions"] = sorted(zip(t["id"].to_pylist(), t["_row_created_at_version"].to_pylist(),
                                     t["_row_last_updated_at_version"].to_pylist()))
    return out


def sequences(ds):
    return [list(RowIdSequence.from_inline_metadata(f.metadata.row_id_meta)) for f in ds.get_fragments()]


def both(lance, tmp_path, steps, **write):
    """Run `steps(module, ds)` on a nanolance and a pylance dataset built the same way."""
    results = []
    for name, module in (("nano", nl), ("py", lance)):
        path = str(tmp_path / f"{name}.lance")
        ds = module.write_dataset(table(), path, enable_stable_row_ids=True, **write)
        results.append((module, steps(module, ds, path)))
    return results


def test_create_append_overwrite(lance, tmp_path):
    def steps(m, ds, path):
        ds = m.write_dataset(table(5, 100), path, mode="append")
        a = state(ds)
        ds = m.write_dataset(table(3, 200), path, mode="overwrite")
        return a, state(ds)

    (_, nano), (_, py) = both(lance, tmp_path, steps, max_rows_per_file=4)
    assert nano == py


def test_update_keeps_ids(lance, tmp_path):
    def steps(m, ds, path):
        ds.update({"v": "-1"}, where="id % 3 = 0")
        return state(ds)

    (_, nano), (_, py) = both(lance, tmp_path, steps, max_rows_per_file=4)
    assert nano == py
    ids = [r[2] for r in nano["rows"]]
    assert sorted(ids) == list(range(10))


def test_delete_then_compact(lance, tmp_path):
    def steps(m, ds, path):
        ds.delete("id in (1, 5, 6)")
        before = state(ds)
        ds.optimize.compact_files(target_rows_per_fragment=100)
        return before, state(ds, with_versions=False)

    (_, nano), (_, py) = both(lance, tmp_path, steps, max_rows_per_file=3)
    assert nano[0] == py[0]
    assert nano[1] == py[1]
    assert [r[2] for r in nano[1]["rows"]] == [r[2] for r in nano[0]["rows"]]


def test_merge_insert(lance, tmp_path):
    def steps(m, ds, path):
        source = pa.table({"id": pa.array([2, 3, 50, 51], pa.int64()), "v": pa.array([-2, -3, 500, 510], pa.int64())})
        ds.merge_insert("id").when_matched_update_all().when_not_matched_insert_all().execute(source)
        return state(ds)

    (_, nano), (_, py) = both(lance, tmp_path, steps, max_rows_per_file=4)
    assert nano == py


def test_take_rows_and_filters(lance, tmp_path):
    def steps(m, ds, path):
        ds.delete("id = 4")
        ds.update({"v": "7"}, where="id = 2")
        ids = ds.to_table(columns=["id"], with_row_id=True).sort_by("id")["_rowid"].to_pylist()
        taken = ds._take_rows([ids[3], 9999, ids[0], ids[0]], columns=["id"]).to_pydict()
        filtered = ds.to_table(columns=["id"], filter=f"_rowid in ({ids[1]}, {ids[2]})").to_pydict()
        return ids, taken, filtered

    (_, nano), (_, py) = both(lance, tmp_path, steps, max_rows_per_file=4)
    assert nano == py


def test_restore_keeps_the_high_water_mark(lance, tmp_path):
    def steps(m, ds, path):
        ds = m.write_dataset(table(4, 100), path, mode="append")
        ds.checkout_version(1)
        ds.restore()
        ds = m.write_dataset(table(2, 300), path, mode="append")
        return state(ds, with_versions=False)

    (_, nano), (_, py) = both(lance, tmp_path, steps)
    assert nano == py


def test_pylance_reads_what_nanolance_wrote(lance, tmp_path):
    path = str(tmp_path / "x.lance")
    ds = nl.write_dataset(table(), path, enable_stable_row_ids=True, max_rows_per_file=4)
    ds.update({"v": "0"}, where="id < 2")
    ds.delete("id = 8")
    ds.optimize.compact_files(target_rows_per_fragment=100)
    other = lance.dataset(path)
    assert state(other) == state(nl.dataset(path))
    assert sequences(nl.dataset(path)) == [list(lance.fragment.RowIdSequence.from_inline_metadata(f.metadata.row_id_meta))
                                           for f in other.get_fragments()]


def test_nanolance_reads_what_pylance_wrote(lance, tmp_path):
    path = str(tmp_path / "y.lance")
    ds = lance.write_dataset(table(), path, enable_stable_row_ids=True, max_rows_per_file=4)
    ds.update({"v": "0"}, where="id < 2")
    ds.delete("id = 8")
    ds.optimize.compact_files(target_rows_per_fragment=100)
    assert state(nl.dataset(path)) == state(ds)


def test_row_id_meta_bytes_match_pylance(lance, tmp_path):
    for values in ([7, 12, 3], list(range(5)), list(range(0, 40, 2)), [1, 2, 3, 5, 6, 9], [4100, 4099, 0]):
        assert RowIdSequence(values).to_inline_metadata().asdict() == \
            lance.fragment.RowIdSequence(values).to_inline_metadata().asdict()


def test_fragment_metadata_round_trips(lance, tmp_path):
    ds = nl.write_dataset(table(), str(tmp_path / "z.lance"), enable_stable_row_ids=True)
    meta = ds.get_fragments()[0].metadata
    assert isinstance(meta.row_id_meta, RowIdMeta)
    assert isinstance(meta.created_at_version_meta, RowDatasetVersionMeta)
    assert pickle.loads(pickle.dumps(meta)) == meta
    assert nl.fragment.FragmentMetadata.from_json(__import__("json").dumps(meta.to_json())) == meta


@pytest.mark.parametrize("engine", ["nano", "py"])
def test_commit_enable_stable_row_ids(lance, tmp_path, engine):
    m = nl if engine == "nano" else lance
    path = str(tmp_path / f"{engine}.lance")
    fragments = m.fragment.write_fragments(table(5), path)
    ds = m.LanceDataset.commit(path, m.LanceOperation.Overwrite(table(5).schema, fragments),
                               enable_stable_row_ids=True)
    more = m.fragment.write_fragments(table(3, 100), path)
    ds = m.LanceDataset.commit(path, m.LanceOperation.Append(more), read_version=ds.version)
    t = ds.to_table(columns=["id"], with_row_id=True)
    assert t["_rowid"].to_pylist() == list(range(8))
    assert [list(RowIdSequence.from_inline_metadata(f.metadata.row_id_meta)) if engine == "nano" else
            list(lance.fragment.RowIdSequence.from_inline_metadata(f.metadata.row_id_meta))
            for f in ds.get_fragments()] == [[0, 1, 2, 3, 4], [5, 6, 7]]


def test_manual_update_keeps_the_row_id(tmp_path):
    ds = nl.write_dataset(pa.table({"id": [1, 2, 3, 4], "v": [10, 20, 30, 40]}), str(tmp_path / "m.lance"),
                          max_rows_per_file=2, enable_stable_row_ids=True)
    before = dict(zip(*[ds.to_table(columns=["id"], with_row_id=True)[c].to_pylist() for c in ("id", "_rowid")]))
    updated = ds.get_fragments()[0].delete("id = 2")
    (new,) = nl.fragment.write_fragments(pa.table({"id": [2], "v": [99]}), str(tmp_path / "m.lance"))
    new.row_id_meta = RowIdSequence([before[2]]).to_inline_metadata()
    ds = nl.LanceDataset.commit(str(tmp_path / "m.lance"),
                                nl.LanceOperation.Update(removed_fragment_ids=[], updated_fragments=[updated],
                                                         new_fragments=[new], fields_modified=[]),
                                read_version=ds.version)
    after = ds.to_table(columns=["id"], with_row_id=True)
    assert dict(zip(after["id"].to_pylist(), after["_rowid"].to_pylist())) == before


# ── indexes: they hold row ids on a dataset with stable row ids ────────────────────────────────────


def indexed_table(n=40):
    import numpy as np

    rng = np.random.default_rng(7)
    words = ["alpha", "beta", "gamma", "delta"]
    return pa.table({
        "id": pa.array(range(n), pa.int64()),
        "k": pa.array([i % 5 for i in range(n)], pa.int64()),
        "text": pa.array([f"{words[i % 4]} {words[(i * 3) % 4]} doc{i}" for i in range(n)]),
        "vec": pa.FixedSizeListArray.from_arrays(pa.array(rng.random(n * 4).astype("float32")), 4),
    })


def index_steps(m, path):
    ds = m.write_dataset(indexed_table(), path, enable_stable_row_ids=True, max_rows_per_file=10)
    ds.create_scalar_index("k", "BTREE")
    ds.create_scalar_index("id", "BITMAP")
    ds.create_scalar_index("text", "INVERTED")
    ds.create_index("vec", "IVF_FLAT", num_partitions=2, metric="l2") if m is nl else \
        ds.create_index("vec", "IVF_FLAT", num_partitions=2, metric="l2")
    out = {}

    def probe(tag):
        out[tag] = {
            "btree": sorted(ds.to_table(columns=["id"], filter="k = 3")["id"].to_pylist()),
            "bitmap": sorted(ds.to_table(columns=["id"], filter="id in (1, 12, 33)")["id"].to_pylist()),
            "fts": sorted(ds.to_table(columns=["id"], full_text_query="gamma")["id"].to_pylist()),
            "knn": ds.to_table(columns=["id"], nearest={"column": "vec", "q": [0.5] * 4, "k": 5})["id"].to_pylist(),
            "rowid": sorted(ds.to_table(columns=["id"], filter="k = 3", with_row_id=True)["_rowid"].to_pylist()),
        }

    probe("built")
    ds.update({"k": "3"}, where="id in (0, 1, 2)")
    ds.delete("id in (5, 6)")
    probe("mutated")
    ds.optimize.compact_files(target_rows_per_fragment=100)
    probe("compacted")
    ds.optimize.optimize_indices()
    probe("optimized")
    return out


def test_indexes_match_pylance(lance, tmp_path):
    nano = index_steps(nl, str(tmp_path / "n.lance"))
    py = index_steps(lance, str(tmp_path / "p.lance"))
    assert nano == py


def test_pylance_searches_indexes_nanolance_built(lance, tmp_path):
    path = str(tmp_path / "x.lance")
    ds = nl.write_dataset(indexed_table(), path, enable_stable_row_ids=True, max_rows_per_file=10)
    ds.create_scalar_index("k", "BTREE")
    ds.create_scalar_index("text", "INVERTED")
    ds.create_index("vec", "IVF_FLAT", num_partitions=2, metric="l2")
    ds.update({"k": "3"}, where="id in (0, 1, 2)")
    ds.delete("id in (5, 6)")
    other = lance.dataset(path)
    for query in (lambda d: sorted(d.to_table(columns=["id"], filter="k = 3")["id"].to_pylist()),
                  lambda d: sorted(d.to_table(columns=["id"], full_text_query="gamma")["id"].to_pylist()),
                  lambda d: d.to_table(columns=["id"], nearest={"column": "vec", "q": [0.5] * 4, "k": 5})["id"].to_pylist()):
        assert query(ds) == query(other)
