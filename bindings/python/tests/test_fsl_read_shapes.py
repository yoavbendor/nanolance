"""Reads of fixed_size_list shapes that pylance 12 writes and nanolance used to refuse (W1k1).

Each shape is written by pylance 12 (data_storage_version 2.2), then read back with nanolance's
``read_table``, ``take`` and a projection. Every result must equal pylance's own read.

- list<fixed_size_list<float32, N>> with null elements inside valid items: the item is valid, one
  of its elements is null. Lance keeps the element validity next to the values (MiniBlock
  FixedSizeList{..., validity}; FullZip for wide items).
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance

ROWS = 3_000


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _list_fsl(dim, rows):
    return pa.table({"v": pa.array(rows, pa.list_(pa.list_(pa.float32(), dim)))})


def _elem_nulls(n):
    # A null element inside valid items, a null item, a null row, an empty list, and a row with
    # only null elements.
    rows = []
    for i in range(n):
        if i % 7 == 0:
            rows.append(None)
        elif i % 5 == 0:
            rows.append([])
        elif i % 3 == 0:
            rows.append([[None, float(i)], None, [1.0, 2.0]])
        elif i % 4 == 0:
            rows.append([[None, None]])
        else:
            rows.append([[float(i), 1.0], [None, 2.0]])
    return _list_fsl(2, rows)


def _all_null_items(n):
    return _list_fsl(2, [[None, None] for _ in range(n)])


def _all_null_rows(n):
    return _list_fsl(2, [None] * n)


def _wide_elem_nulls(n):
    # Items wide enough that Lance writes them as FullZip rather than MiniBlock.
    rows = []
    for i in range(n):
        if i % 6 == 0:
            rows.append(None)
            continue
        item = [float(i + j) for j in range(256)]
        item[(i * 7) % 256] = None
        rows.append([item, None if i % 2 else [None] * 256])
    return _list_fsl(256, rows)


SHAPES = {
    "list_fsl_elem_nulls": lambda n=ROWS: _elem_nulls(n),
    "list_fsl_all_null_items": lambda n=ROWS: _all_null_items(n),
    "list_fsl_all_null_rows": lambda n=ROWS: _all_null_rows(n),
    "list_fsl_wide_elem_nulls": lambda n=ROWS: _wide_elem_nulls(n),
}


def _with_plain_column(table):
    """Adds a plain int64 column, so the projection test has something to leave out."""
    return table.append_column("k", pa.array(range(table.num_rows), pa.int64()))


def _check_equal_to_pylance(lance_mod, path, table):
    expected = lance_mod.dataset(str(path)).to_table()
    assert expected.to_pydict() == table.to_pydict()  # pylance wrote what we meant it to write
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == expected.to_pydict()
    return expected


@pytest.mark.parametrize("name", sorted(SHAPES))
def test_read_matches_pylance(lance_mod, tmp_path, name):
    table = _with_plain_column(SHAPES[name]())
    path = tmp_path / f"{name}.lance"
    lance_mod.write_dataset(table, str(path), data_storage_version="2.2")
    expected = _check_equal_to_pylance(lance_mod, path, table)

    # The projection that leaves the plain column out, and the one that keeps only the shape.
    assert pa.table(nanolance.read_table(path, columns=["v"])).to_pydict() == expected.select(["v"]).to_pydict()
    assert pa.table(nanolance.read_table(path, columns=["k"])).to_pydict() == expected.select(["k"]).to_pydict()


@pytest.mark.parametrize("name", sorted(SHAPES))
def test_take_matches_pylance(lance_mod, tmp_path, name):
    table = _with_plain_column(SHAPES[name]())
    path = tmp_path / f"{name}.lance"
    lance_mod.write_dataset(table, str(path), data_storage_version="2.2")
    expected = lance_mod.dataset(str(path))
    idx = [ROWS - 1, 0, 7, 7, 1_000, 3, 5, 14]
    got = pa.table(nanolance.take(str(path), idx))
    assert got.to_pydict() == expected.take(idx).to_pydict()
    got = pa.table(nanolance.take(str(path), idx, columns=["v"]))
    assert got.to_pydict() == expected.take(idx, columns=["v"]).to_pydict()


def test_multi_page_list_fsl_with_element_nulls(lance_mod, tmp_path):
    n = 300_000
    table = _with_plain_column(_elem_nulls(n))
    path = tmp_path / "big.lance"
    lance_mod.write_dataset(table, str(path), data_storage_version="2.2")
    expected = _check_equal_to_pylance(lance_mod, path, table)
    idx = [n - 1, 0, 123_456, 299_999, 7]
    assert pa.table(nanolance.take(str(path), idx)).to_pydict() == expected.take(idx).to_pydict()
