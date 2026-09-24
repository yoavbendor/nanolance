"""List, map and null-struct columns WRITTEN by nanolance (roadmap Phase D), read back by both readers.

The shapes are the read matrix's (test_lance_lists.py): the same values pylance writes and nanolance
must read are here written by nanolance and read by pylance -- the oracle -- and by nanolance. Each
also goes through the paths that re-cut a nested column on the way in:

  * sliced batches (`Table.to_batches()` hands out views addressed through `ArrowArray::offset` at
    every level -- a list's child is not row-aligned with the batch, a struct's children are not
    sliced with it);
  * several fragments (`max_rows_per_fragment`), so layers are rebased across batches;
  * long rows, so a page is cut into several at row boundaries.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance
from tests.test_lance_lists import NULL_STRUCTS, SHAPES


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


ALL = {**SHAPES, **{f"null_struct_{k}": v for k, v in NULL_STRUCTS.items()}}
# Not writable yet, and refused by name: a list of vectors (a FixedSizeList under a list layer).
NOT_WRITABLE = {"null_struct_vectors"}


def _both_readers_agree(lance_mod, path, table):
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == table.to_pydict(), "nanolance re-read mismatch"
    assert lance_mod.dataset(str(path)).to_table().to_pydict() == table.to_pydict(), "stock Lance re-read mismatch"


@pytest.mark.parametrize("name", sorted(set(ALL) - NOT_WRITABLE))
def test_nested_column_round_trips(lance_mod, tmp_path, name):
    table = pa.table({"id": pa.array(range(len(ALL[name]())), pa.int64()), "c": ALL[name]()})
    path = tmp_path / f"{name}.lance"
    nanolance.write_table(table, path)
    _both_readers_agree(lance_mod, path, table)


@pytest.mark.parametrize("name", sorted(set(ALL) - NOT_WRITABLE))
def test_nested_column_survives_sliced_batches_and_fragments(lance_mod, tmp_path, name):
    column = ALL[name]()
    table = pa.table({"id": pa.array(range(len(column)), pa.int64()), "c": column})
    path = tmp_path / f"{name}.lance"
    with nanolance.LanceWriter(path, max_rows_per_fragment=1_100) as writer:
        for batch in table.to_batches(max_chunksize=437):
            writer.write_batch(batch)
    _both_readers_agree(lance_mod, path, table)
    if table.num_rows < 1_115:
        return
    # And a range across a fragment boundary, through both readers' own slicing.
    expected = table.slice(1_090, 25).to_pydict()
    assert pa.table(nanolance.read_table(path, offset=1_090, length=25)).to_pydict() == expected
    assert lance_mod.dataset(str(path)).to_table(offset=1_090, limit=25).to_pydict() == expected


def test_long_rows_split_across_pages(lance_mod, tmp_path):
    """Rows whose items overflow one page's level budget are cut into several pages, at row
    boundaries -- 40,000-item rows exceed the 32,000 levels a raw-u16 page can hold."""
    lengths = [40_000 if i % 5 == 0 else i % 7 for i in range(60)]
    column = pa.array([list(range(n)) for n in lengths], pa.list_(pa.int32()))
    table = pa.table({"c": column})
    path = tmp_path / "long.lance"
    with pytest.raises(Exception) as excinfo:
        nanolance.write_table(table, path)
    assert "one row can hold at most" in str(excinfo.value)
    # Rows under the limit, but many of them: several pages.
    column = pa.array([list(range(i % 3000)) for i in range(200)], pa.list_(pa.int32()))
    table = pa.table({"c": column})
    nanolance.write_table(table, path)
    _both_readers_agree(lance_mod, path, table)


def test_nested_columns_beside_flat_ones_with_compression(lance_mod, tmp_path):
    n = 3_000
    table = pa.table(
        {
            "a": pa.array(range(n), pa.int64()),
            "tags": SHAPES["string"](),
            "m": SHAPES["map_nulls"](),
            "s": pa.array([f"x{i % 50}" for i in range(n)]),
            "ls": SHAPES["list_of_struct_nulls"](),
        }
    )
    path = tmp_path / "mixed.lance"
    nanolance.write_table(table, path, compression=True)
    _both_readers_agree(lance_mod, path, table)
    for name in table.column_names:
        got = lance_mod.dataset(str(path)).to_table(columns=[name]).column(0).to_pylist()
        assert got == table.column(name).to_pylist(), f"stock Lance mis-read column {name!r} on its own"


def test_a_list_of_vectors_is_refused_by_name(tmp_path):
    table = pa.table({"c": pa.array([[[1.0, 2.0]] * (i % 3) for i in range(50)], pa.list_(pa.list_(pa.float32(), 2)))})
    with pytest.raises(Exception) as excinfo:
        nanolance.write_table(table, tmp_path / "x.lance")
    assert "cannot be written yet" in str(excinfo.value)


def test_list_children_with_their_own_offset(lance_mod, tmp_path):
    """A list array built over a SLICED child: the child's own `offset` is non-zero, and the list's
    offsets are logical indices into that child. `to_batches()` never produces this (it offsets the
    list, not its child), which is why it needs its own test -- ingest used to be one `+ offset` away
    from reading the wrong items without any test noticing."""
    values = pa.array([f"v{i}" for i in range(200)]).slice(17)
    offsets = pa.array([0, 2, 2, 5, 9, 9, 12], pa.int32())
    inner = pa.ListArray.from_arrays(offsets, values)
    # And one more level: a list whose child is that (already offset) list, itself sliced.
    outer = pa.ListArray.from_arrays(pa.array([0, 1, 3, 3, 5], pa.int32()), inner.slice(1))
    table = pa.table({"inner": pa.chunked_array([inner.slice(1, 4)]), "outer": pa.chunked_array([outer.slice(0, 4)])})
    path = tmp_path / "offsets.lance"
    nanolance.write_table(table, path)
    _both_readers_agree(lance_mod, path, table)
