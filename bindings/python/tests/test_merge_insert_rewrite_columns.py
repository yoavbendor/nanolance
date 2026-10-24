# SPDX-License-Identifier: Apache-2.0
"""merge_insert(...).write_mode("rewrite_columns"): the source's columns are patched into the fragments that
hold the matched rows -- no fragment added, removed or renumbered, the other columns untouched -- and pylance
reads the result."""

import pyarrow as pa
import pytest

from tests.support import require_pylance

import nanolance.lance as nl


def _dataset(path, **kwargs):
    table = pa.table({"a": range(10), "k": [i % 3 for i in range(10)], "b": range(10), "c": range(10, 20)})
    return nl.write_dataset(table, path, max_rows_per_file=5, **kwargs)


@pytest.mark.parametrize("on", [["a"], ["a", "k"]])
def test_patches_columns_in_place(tmp_path, on):
    lance = require_pylance()
    path = str(tmp_path / "ds")
    ds = _dataset(path)
    before = [(f.fragment_id, [d.path for d in f.data_files()]) for f in ds.get_fragments()]
    source = pa.table({"a": [3, 4, 6, 42], "k": [0, 1, 0, 0], "b": [30, 40, 60, 420]})
    stats = ds.merge_insert(on).when_matched_update_all().write_mode("rewrite_columns").execute(source)
    assert stats == {"num_inserted_rows": 0, "num_updated_rows": 3, "num_deleted_rows": 0}
    after = nl.dataset(path)
    # Same fragments with their original files; one new file each, holding `b` alone.
    for (fid, files), f in zip(before, after.get_fragments()):
        assert f.fragment_id == fid
        assert [d.path for d in f.data_files()][: len(files)] == files
    b = [0, 1, 2, 30, 40, 5, 60, 7, 8, 9]
    expected = pa.table({"a": range(10), "k": [i % 3 for i in range(10)], "b": b, "c": range(10, 20)})
    assert after.to_table().sort_by("a").equals(expected)
    assert lance.dataset(path).to_table().sort_by("a").equals(expected)


def test_refused_with_inserts_and_full_schema(tmp_path):
    ds = _dataset(str(tmp_path / "ds"))
    with pytest.raises(OSError, match="RewriteColumns"):
        ds.merge_insert("a").when_matched_update_all().when_not_matched_insert_all().write_mode(
            "rewrite_columns").execute(pa.table({"a": [1, 11], "b": [1, 2]}))
    full = ds.to_table().slice(0, 2)
    with pytest.raises(OSError, match="RewriteColumns"):
        ds.merge_insert("a").when_matched_update_all().write_mode("rewrite_columns").execute(full)


def test_uncommitted_and_row_versions(tmp_path):
    path = str(tmp_path / "ds")
    ds = _dataset(path, enable_stable_row_ids=True)
    txn, stats = ds.merge_insert("a").when_matched_update_all().write_mode("rewrite_columns").execute_uncommitted(
        pa.table({"a": [1, 8], "b": [100, 800]}))
    assert stats["num_updated_rows"] == 2
    assert txn.operation.update_mode == "rewrite_columns"
    assert nl.dataset(path).version == ds.version  # nothing committed yet
    done = nl.LanceDataset.commit(path, txn.operation, read_version=txn.read_version)
    got = done.to_table(columns=["a", "b", "_row_last_updated_at_version"]).sort_by("a")
    assert got.column("b").to_pylist() == [0, 100, 2, 3, 4, 5, 6, 7, 800, 9]
    assert got.column("_row_last_updated_at_version").to_pylist() == [1, 2, 1, 1, 1, 1, 1, 1, 2, 1]


def test_duplicate_source_key_refused(tmp_path):
    ds = _dataset(str(tmp_path / "ds"))
    with pytest.raises(ValueError, match="more than one"):
        ds.merge_insert("a").when_matched_update_all().write_mode("rewrite_columns").execute(
            pa.table({"a": [2, 2], "b": [1, 2]}))
