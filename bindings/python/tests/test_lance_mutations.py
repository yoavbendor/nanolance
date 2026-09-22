"""Reading Lance datasets that pylance has MODIFIED, not just created.

Every other stock-Lance test in this suite writes the dataset exactly once. That turned out to be a
load-bearing coincidence: a single-version dataset has exactly one manifest, so picking "the
numerically largest filename in _versions/" is right by accident.

It is wrong on purpose for every multi-version dataset. Lance names manifests under two schemes that
sort in OPPOSITE directions (`rust/lance-table/src/io/commit.rs`, `ManifestNamingScheme`):

    V1: _versions/{version}.manifest              <- what nanolance writes
    V2: _versions/{u64::MAX - version}.manifest   <- what pylance writes, 20-digit zero-padded,
                                                     so the NEWEST version sorts FIRST

so nanolance read back **version 1** of anything pylance had appended to, overwritten, updated or
deleted from -- silently, with no error. An append returned only the first batch; an overwrite
returned the pre-overwrite rows; a delete returned the deleted rows.

These tests exercise the dataset lifecycle rather than the column encodings, which is the axis the
rest of the suite does not cover.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture
def lance_mod():
    return require_pylance()


def _nl(path):
    return pa.table(nanolance.read_table(path))


def test_append_sees_every_fragment(lance_mod, tmp_path):
    path = str(tmp_path / "appended.lance")
    lance_mod.write_dataset(pa.table({"a": pa.array([1, 2], type=pa.int64())}), path)
    lance_mod.write_dataset(pa.table({"a": pa.array([3, 4], type=pa.int64())}), path, mode="append")

    expected = lance_mod.dataset(path).to_table()
    assert expected.num_rows == 4
    assert _nl(path).to_pydict() == expected.to_pydict()
    assert nanolance.count_rows(path) == 4


def test_overwrite_sees_the_new_data_not_the_old(lance_mod, tmp_path):
    """The sharpest case: the superseded fragment is still on disk, so reading the wrong manifest
    returns plausible-looking data of the wrong length rather than failing."""
    path = str(tmp_path / "overwritten.lance")
    lance_mod.write_dataset(pa.table({"a": pa.array([1, 2, 3], type=pa.int64())}), path)
    lance_mod.write_dataset(pa.table({"a": pa.array([9, 8], type=pa.int64())}), path, mode="overwrite")

    expected = lance_mod.dataset(path).to_table()
    assert expected.column(0).to_pylist() == [9, 8]
    assert _nl(path).to_pydict() == expected.to_pydict()
    assert nanolance.count_rows(path) == 2


def test_many_versions_resolve_to_the_latest(lance_mod, tmp_path):
    """Several versions, so "largest filename" and "newest version" disagree by more than one."""
    path = str(tmp_path / "many.lance")
    lance_mod.write_dataset(pa.table({"a": pa.array([0], type=pa.int64())}), path)
    for i in range(1, 6):
        lance_mod.write_dataset(pa.table({"a": pa.array([i], type=pa.int64())}), path, mode="append")

    expected = lance_mod.dataset(path).to_table()
    assert expected.column(0).to_pylist() == list(range(6))
    assert _nl(path).to_pydict() == expected.to_pydict()


def test_schema_and_row_count_come_from_the_latest_version(lance_mod, tmp_path):
    """count_rows and read_schema answer from the manifest, so they take the same wrong turn."""
    path = str(tmp_path / "meta.lance")
    lance_mod.write_dataset(pa.table({"a": pa.array([1, 2, 3], type=pa.int64())}), path)
    lance_mod.write_dataset(pa.table({"a": pa.array([4], type=pa.int64())}), path, mode="append")

    assert nanolance.count_rows(path) == lance_mod.dataset(path).count_rows() == 4
    assert pa.schema(nanolance.read_schema(path)).names == ["a"]


def test_a_nanolance_append_onto_a_pylance_dataset_still_reads(lance_mod, tmp_path):
    """A dataset can legitimately hold BOTH naming schemes: pylance created it (V2), nanolance
    appended to it (V1). The version is scheme-independent, so both spellings have to be tried."""
    path = tmp_path / "mixed.lance"
    lance_mod.write_dataset(pa.table({"a": pa.array([1, 2], type=pa.int64())}), str(path))
    nanolance.write_table(
        pa.table({"a": pa.array([3, 4], type=pa.int64())}),
        path,
        options=nanolance.WriteOptions(append=True),
    )

    got = _nl(path)
    assert got.column(0).to_pylist() == [1, 2, 3, 4]
    assert lance_mod.dataset(str(path)).to_table().column(0).to_pylist() == [1, 2, 3, 4]
