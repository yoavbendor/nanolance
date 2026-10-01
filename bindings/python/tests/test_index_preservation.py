"""A dataset's indices survive the commits nanolance makes.

nanolance builds no index, but it writes to datasets pylance indexed: append, delete, update,
merge_insert, column changes, compaction. Each commit used to drop the manifest's index section, so
pylance saw no indices afterwards and every indexed query became a full scan. nanolance now carries
them on by Lance's own rules (src/index_maintenance.cpp): an index keeps the fragments it covers, a
fragment whose rows it no longer describes leaves its coverage, and an index on a dropped column goes.

Each test indexes a dataset with pylance (BTree, Bitmap, full-text and IVF_PQ), changes it with
nanolance, and checks with pylance: which indices exist, which fragments they cover, that every
indexed query returns what a plain scan returns, and that the dataset validates.
"""

from __future__ import annotations

import shutil

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

ALL = ["cat_idx", "id_idx", "text_idx", "vec_idx"]

# list_indices reports each index's fragment ids, which is what these tests are about.
pytestmark = pytest.mark.filterwarnings("ignore:The 'list_indices' method is deprecated")


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _table(n: int, start: int = 0, seed: int = 0) -> pa.Table:
    rng = np.random.default_rng(seed + start)
    ids = np.arange(start, start + n)
    return pa.table({
        "id": pa.array(ids),
        "cat": pa.array([f"c{i % 7}" for i in ids]),
        "text": pa.array([f"hello world number {i} " + ("apple" if i % 5 == 0 else "pear") for i in ids]),
        "vec": pa.FixedSizeListArray.from_arrays(pa.array(rng.standard_normal(n * 16, dtype=np.float32)), 16),
    })


@pytest.fixture(scope="module")
def indexed(lance, tmp_path_factory):
    """Three fragments of 1000 rows, every column indexed."""
    path = tmp_path_factory.mktemp("indexed") / "seed.lance"
    lance.write_dataset(_table(3_000), str(path), max_rows_per_file=1_000)
    ds = lance.dataset(str(path))
    ds.create_scalar_index("id", "BTREE")
    ds.create_scalar_index("cat", "BITMAP")
    ds.create_scalar_index("text", "INVERTED")
    ds.create_index("vec", "IVF_PQ", num_partitions=4, num_sub_vectors=4)
    return path


@pytest.fixture
def ds_path(indexed, tmp_path):
    path = tmp_path / "t.lance"
    shutil.copytree(indexed, path)
    return str(path)


def _coverage(lance, path):
    return {i["name"]: sorted(i["fragment_ids"]) for i in lance.dataset(path).list_indices()}


def _check_queries(lance, path):
    """Every indexed query returns what a scan without the index returns."""
    ds = lance.dataset(path)
    ds.validate()
    names = ds.schema.names
    if "id" in names:
        filters = ["id = 5", "id = 3100", "id < 40", "id >= 2990 AND id < 3020"]
        if "cat" in names:
            filters += ["cat = 'c3'", "cat = 'zz'"]
        for f in filters:
            with_index = sorted(ds.to_table(filter=f, columns=["id"]).column("id").to_pylist())
            without = sorted(ds.to_table(filter=f, columns=["id"], use_scalar_index=False).column("id").to_pylist())
            assert with_index == without, f
    text = "text" if "text" in names else "body" if "body" in names else None
    if text and "id" in names and any(i["name"] == "text_idx" for i in ds.list_indices()):
        fts = sorted(ds.to_table(full_text_query="apple", columns=["id"]).column("id").to_pylist())
        scan = sorted(ds.to_table(filter=f"{text} LIKE '%apple%'", columns=["id"]).column("id").to_pylist())
        assert fts == scan
    if "vec" in names and "id" in names:
        # The newest row is found whether or not the index covers its fragment.
        last = ds.to_table(columns=["id", "vec"], offset=ds.count_rows() - 1, limit=1)
        q = np.array(last.column("vec")[0].as_py(), dtype=np.float32)
        hits = ds.to_table(nearest={"column": "vec", "q": q, "k": 5}, columns=["id"]).column("id").to_pylist()
        assert last.column("id")[0].as_py() in hits


def test_append_keeps_every_index(lance, ds_path):
    nl.write_dataset(_table(500, 3_000), ds_path, mode="append")
    assert _coverage(lance, ds_path) == {name: [0, 1, 2] for name in ALL}  # the new fragment is not covered
    _check_queries(lance, ds_path)


def test_delete_keeps_every_index(lance, ds_path):
    nl.dataset(ds_path).delete("id % 10 = 0")
    nl.dataset(ds_path).delete("id < 1000")  # fragment 0 entirely
    assert set(_coverage(lance, ds_path)) == set(ALL)
    _check_queries(lance, ds_path)


def test_update_and_merge_insert_keep_every_index(lance, ds_path):
    nl.dataset(ds_path).update({"cat": "'zz'"}, where="id < 50")
    (nl.dataset(ds_path).merge_insert("id").when_matched_update_all().when_not_matched_insert_all()
     .execute(_table(100, 2_950, seed=9)))
    assert _coverage(lance, ds_path) == {name: [0, 1, 2] for name in ALL}
    _check_queries(lance, ds_path)


def test_column_changes(lance, ds_path):
    nl.dataset(ds_path).add_columns({"double": "id * 2"})
    nl.dataset(ds_path).alter_columns({"path": "text", "name": "body"})  # a rename keeps its index
    assert set(_coverage(lance, ds_path)) == set(ALL)
    nl.dataset(ds_path).drop_columns(["cat"])
    assert set(_coverage(lance, ds_path)) == {"id_idx", "text_idx", "vec_idx"}
    nl.dataset(ds_path).alter_columns({"path": "id", "data_type": pa.int32()})  # new data: its index goes
    assert set(_coverage(lance, ds_path)) == {"text_idx", "vec_idx"}
    _check_queries(lance, ds_path)


def test_compaction_narrows_coverage(lance, ds_path):
    nl.dataset(ds_path).delete("id >= 1000 AND id < 1300")
    nl.dataset(ds_path).optimize.compact_files(target_rows_per_fragment=500, materialize_deletions_threshold=0.1)
    ds = lance.dataset(ds_path)
    assert sorted(f.fragment_id for f in ds.get_fragments()) == [0, 2, 3]  # 1 rewritten as 3, in id order
    assert _coverage(lance, ds_path) == {name: [0, 2] for name in ALL}
    _check_queries(lance, ds_path)


def test_overwrite_drops_the_indices(lance, ds_path):
    nl.write_dataset(_table(100, 9_000), ds_path, mode="overwrite")
    assert _coverage(lance, ds_path) == {}


def test_new_fragments_never_take_an_indexed_id(lance, ds_path):
    nl.dataset(ds_path).delete("id >= 2000")  # the highest fragment, 2, goes
    nl.write_dataset(_table(10, 5_000), ds_path, mode="append")
    ids = sorted(f.fragment_id for f in lance.dataset(ds_path).get_fragments())
    assert 2 not in ids  # covered by every index: a new fragment there would pass as indexed
    _check_queries(lance, ds_path)


def test_pylance_reindexes_after_nanolance(lance, ds_path):
    nl.write_dataset(_table(500, 3_000), ds_path, mode="append")
    lance.dataset(ds_path).optimize.optimize_indices()
    assert _coverage(lance, ds_path) == {name: [0, 1, 2, 3] for name in ALL}
    _check_queries(lance, ds_path)


def test_restore_brings_back_that_versions_indices(lance, ds_path):
    nl.dataset(ds_path).drop_columns(["cat"])
    version = lance.dataset(ds_path).version
    nl.dataset(ds_path, version=version - 1).restore()
    assert set(_coverage(lance, ds_path)) == set(ALL)
    _check_queries(lance, ds_path)
