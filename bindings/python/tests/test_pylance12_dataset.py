"""Dataset behaviour pylance 12 has and nanolance matches: slice bounds, strict batch sizes, the
file listings (tracked_files, all_files), non-UTF-8 schema metadata, and new columns that clash with
existing ones. Each test compares with pylance where it can.
"""

from __future__ import annotations

import pickle

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


def test_slice_rejects_bad_bounds(tmp_path):
    ds = nl.write_dataset(pa.table({"a": range(10)}), tmp_path / "ds")
    with pytest.raises(ValueError, match="start must be non-negative, got -1"):
        ds.slice(-1, 5)
    with pytest.raises(ValueError, match=r"end \(5\) must be >= start \(10\)"):
        ds.slice(10, 5)


def test_to_batches_strict_batch_size(tmp_path):
    table = pa.table({"a": range(96)})
    ds = nl.write_dataset(table, tmp_path / "ds", max_rows_per_file=32)
    sizes = [b.num_rows for b in ds.to_batches(batch_size=5, strict_batch_size=True)]
    assert sizes == [5] * 19 + [1]
    # Without strict_batch_size, batches stop at chunk boundaries.
    assert sum(sizes) == 96


def test_file_listings_match_pylance(tmp_path):
    lance = require_pylance()
    table = pa.table({"x": [1, 2, 3]})
    ours = nl.write_dataset(table, tmp_path / "ours")
    ours.delete("x = 2")
    theirs = lance.write_dataset(table, tmp_path / "theirs")
    theirs.delete("x = 2")

    schema = ours.tracked_files().schema
    assert [f.name for f in schema] == ["version", "base_uri", "path", "type"]
    assert {r["type"] for r in ours.tracked_files().read_all().to_pylist()} == \
        {r["type"] for r in theirs.tracked_files().read_all().to_pylist()}

    listed = ours.all_files().read_all()
    assert listed.schema.names == ["base_uri", "path", "size_bytes", "last_modified"]
    assert all(s > 0 for s in listed.column("size_bytes").to_pylist())


def test_metadata_value_must_be_utf8(tmp_path):
    table = pa.table({"a": [1]})
    with pytest.raises(ValueError, match="not valid utf8"):
        nl.write_dataset(table.replace_schema_metadata({"foo": pickle.dumps("foo")}), tmp_path / "bad")
    nl.write_dataset(table.replace_schema_metadata({"foo": b"bar"}), tmp_path / "ok")


def test_add_columns_conflicts_with_existing_column(tmp_path):
    ds = nl.write_dataset(pa.table({"id": [1, 2, 4]}), tmp_path / "ds")
    with pytest.raises(OSError, match=r"Type conflicts between id\(Int64\) and id\(Float32\)"):
        ds.add_columns(pa.field("id", pa.float32()))
    with pytest.raises(OSError, match="Column id already exists in the dataset"):
        ds.add_columns(pa.field("id", pa.int64()))
