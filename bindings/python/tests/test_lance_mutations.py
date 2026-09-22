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


# ── Deletion files ───────────────────────────────────────────────────────────────────────────────
# Lance records deleted rows in `_deletions/{fragment}-{read_version}-{id}.{arrow|bin}` and picks the
# shape by density: an Arrow IPC file of u32 offsets when sparse, a roaring bitmap above 5000. Both
# are parsed directly (see src/deletion_vector.cpp); neither was read at all before, so a dataset
# that had ever had delete() called on it returned the deleted rows with no error.


def _deleted(lance_mod, tmp_path, name, rows, predicate):
    path = str(tmp_path / f"{name}.lance")
    lance_mod.write_dataset(pa.table({"id": pa.array(range(rows), type=pa.int64())}), path)
    lance_mod.dataset(path).delete(predicate)
    return path


def test_sparse_deletions_arrow_format(lance_mod, tmp_path):
    """Under 5000 deleted rows: an Arrow IPC file, zstd-compressed body."""
    path = _deleted(lance_mod, tmp_path, "sparse", 100, "id < 10")
    expected = lance_mod.dataset(path).to_table()
    assert expected.num_rows == 90
    assert _nl(path).to_pydict() == expected.to_pydict()
    assert nanolance.count_rows(path) == 90


def test_dense_deletions_roaring_bitmap_format(lance_mod, tmp_path):
    """Above 5000 deleted rows Lance switches to a roaring bitmap -- a different parser entirely."""
    path = _deleted(lance_mod, tmp_path, "dense", 20_000, "id < 9000")
    expected = lance_mod.dataset(path).to_table()
    assert expected.num_rows == 11_000
    assert _nl(path).to_pydict() == expected.to_pydict()
    assert nanolance.count_rows(path) == 11_000


def test_scattered_deletions_keep_the_surviving_rows_in_order(lance_mod, tmp_path):
    """Deleting every third row exercises the gather rather than a prefix trim."""
    path = _deleted(lance_mod, tmp_path, "scattered", 500, "id % 3 == 0")
    expected = lance_mod.dataset(path).to_table()
    assert _nl(path).column("id").to_pylist() == expected.column("id").to_pylist()


def test_deletions_compose_with_a_row_range(lance_mod, tmp_path):
    """A row range counts LOGICAL rows -- the ones a read returns -- not physical ones.

    Deletions have to be applied before the range is, or `offset` would address rows that are not
    there any more and a range would silently return the wrong window.
    """
    path = _deleted(lance_mod, tmp_path, "ranged", 1_000, "id % 4 == 0")
    full = _nl(path)
    for offset, length in ((0, 10), (7, 23), (100, 250), (740, None)):
        got = pa.table(nanolance.read_table(path, offset=offset, length=length))
        assert got.to_pydict() == full.slice(
            offset, full.num_rows - offset if length is None else length
        ).to_pydict(), f"offset={offset} length={length}"


def test_deletions_compose_with_projection_and_nulls(lance_mod, tmp_path):
    """Deletions, a projection and a nullable column together -- the validity bitmap is the one
    buffer the deletion gather has to re-pack bit by bit.

    n is 2000 rather than a few hundred on purpose: at some sizes pylance encodes a nullable string
    column's definition levels as InlineBitpacking(16), which nanolance refuses by name (an
    unrelated, pre-existing read gap -- see PROGRESS). Using a size that avoids it keeps this test
    about deletions instead of failing for a reason it is not testing.
    """
    path = str(tmp_path / "proj.lance")
    n = 2_000
    lance_mod.write_dataset(
        pa.table(
            {
                "id": pa.array(range(n), type=pa.int64()),
                "s": pa.array([None if i % 7 == 0 else f"v{i}" for i in range(n)], type=pa.string()),
            }
        ),
        path,
    )
    lance_mod.dataset(path).delete("id % 5 == 0")
    expected = lance_mod.dataset(path).to_table()

    assert _nl(path).to_pydict() == expected.to_pydict()
    assert pa.table(nanolance.read_table(path, ["s"])).column("s").to_pylist() == expected.column("s").to_pylist()


# ── Fragments spanning several data files ────────────────────────────────────────────────────────


def test_add_columns_puts_a_column_in_a_second_file_of_the_same_fragment(lance_mod, tmp_path):
    """`add_columns` writes the computed column to its own file BESIDE the original.

    Those files are not more rows, they are more COLUMNS of the same rows. Treating each as its own
    batch dropped every column after the first file's -- and the schema mapping compounded it by
    deriving "is this field materialized" from `files[0]` alone, so the added column looked absent.
    """
    path = str(tmp_path / "added.lance")
    lance_mod.write_dataset(pa.table({"a": pa.array(range(10), type=pa.int64())}), path)
    lance_mod.dataset(path).add_columns({"b": "a * 2"})

    expected = lance_mod.dataset(path).to_table()
    assert expected.column_names == ["a", "b"]
    got = _nl(path)
    assert got.column_names == ["a", "b"]
    assert got.to_pydict() == expected.to_pydict()
    assert pa.table(nanolance.read_table(path, ["b"])).column("b").to_pylist() == [i * 2 for i in range(10)]
