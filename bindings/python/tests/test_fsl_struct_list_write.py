"""Writes of a list, large list or map under fixed_size_list<struct, N> (W1k5), read back by pylance 12 and by nanolance.

Each case is one column, FixedSizeList(2 x Struct(id int32, values list|large_list<int32> | map<utf8, int32>)), with
nulls at every level: null rows, null struct items, null lists, empty lists, null list items, null ids and valid values.
Lance 12 counts a fixed_size_list<struct> column's entries against the other columns of its file, so each case is a
table of that one column. pylance reads the file whole, by take and by row range; nanolance reads it the same way;
all equal the pyarrow input.
"""

from __future__ import annotations

import numpy as np
import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


SHAPES = {
    "list": (pa.list_(pa.int32()), "values"),
    "large_list": (pa.large_list(pa.int32()), "values"),
    "map": (pa.map_(pa.string(), pa.int32()), "props"),
}


def _value(shape, seed):
    """A list (or map) value for this seed: empty, null, or with a null item, by the seed's residue."""
    if shape == "map":
        if seed % 11 == 0:
            return None
        if seed % 5 == 0:
            return []
        return [(f"k{seed}", seed), (f"j{seed}", None if seed % 3 == 0 else seed + 1)]
    if seed % 11 == 0:
        return None
    if seed % 5 == 0:
        return []
    return [seed, None if seed % 3 == 0 else seed + 1]


def _rows(shape, n, nulls=True, slots=2, seed0=0):
    rows = []
    for i in range(n):
        if nulls and i % 9 == 0:
            rows.append(None)
            continue
        items = []
        for j in range(slots):
            k = seed0 + slots * i + j
            if nulls and k % 13 == 0:
                items.append(None)
                continue
            key = "props" if shape == "map" else "values"
            items.append({"id": None if nulls and k % 7 == 0 else k, key: _value(shape, k)})
        rows.append(items)
    return rows


def _table(shape, n, nulls=True, slots=2, seed0=0):
    value_type, name = SHAPES[shape]
    struct = pa.struct([("id", pa.int32()), (name, value_type)])
    return pa.table({"v": pa.array(_rows(shape, n, nulls, slots, seed0), pa.list_(struct, slots))})


CASES = {
    "list_nulls": lambda: _table("list", 2000),
    "list_no_nulls": lambda: _table("list", 1500, nulls=False),
    "large_list_nulls": lambda: _table("large_list", 2000),
    "map_nulls": lambda: _table("map", 2000),
    "map_no_nulls": lambda: _table("map", 1500, nulls=False),
    "list_n3": lambda: _table("list", 1200, slots=3),
    "list_n20": lambda: _table("list", 400, slots=20),
    "list_first_row_null": lambda: pa.table(
        {"v": pa.array([None] + _rows("list", 10, seed0=100), pa.list_(pa.struct([("id", pa.int32()), ("values", pa.list_(pa.int32()))]), 2))}
    ),
    "list_sliced": lambda: _table("list", 3000).slice(101, 1500),
}


def _check(lance_mod, tmp_path, name):
    table = CASES[name]()
    path = str(tmp_path / f"{name}.lance")
    nanolance.write_table(table, path)
    ds = lance_mod.dataset(path)
    assert ds.data_storage_version == "2.2"
    expected = table.to_pydict()
    assert ds.to_table().to_pydict() == expected

    got = pa.table(nanolance.read_table(path))
    assert got.to_pydict() == expected

    n = table.num_rows
    picks = [0, 1, 2, n // 2, n - 1]
    assert pa.table(nanolance.take(path, picks)).to_pydict() == {"v": [expected["v"][r] for r in picks]}
    assert ds.take(picks).to_pydict() == {"v": [expected["v"][r] for r in picks]}

    for offset, length in [(0, min(7, n)), (n // 3, min(401, n - n // 3)), (max(0, n - 5), 5)]:
        ranged = pa.table(nanolance.read_table(path, offset=offset, length=length))
        assert ranged.to_pydict() == {"v": expected["v"][offset : offset + length]}
        assert ds.to_table(offset=offset, limit=length).to_pydict() == ranged.to_pydict()


@pytest.mark.parametrize("name", sorted(CASES))
def test_fsl_struct_list_round_trip(lance_mod, tmp_path, name):
    _check(lance_mod, tmp_path, name)


def test_fsl_struct_list_many_rows(lance_mod, tmp_path):
    rng = np.random.default_rng(3)
    table = _table("list", 120_000)
    path = str(tmp_path / "many.lance")
    nanolance.write_table(table, path)
    expected = table.to_pydict()
    assert lance_mod.dataset(path).to_table().to_pydict() == expected
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected
    picks = sorted(rng.choice(table.num_rows, size=50, replace=False).tolist())
    assert pa.table(nanolance.take(path, picks)).to_pydict() == {"v": [expected["v"][r] for r in picks]}


# The encodings nanolance and pylance choose for a short column differ on purpose: pylance run-length encodes the
# definition levels of 11 rows with two runs, where nanolance bitpacks them (the allowance test_fsl_struct_write makes).
LAYOUT_DIFFERS = {"list_first_row_null"}


@pytest.mark.parametrize("name", sorted(CASES))
def test_fsl_struct_list_layout_as_pylance(lance_mod, tmp_path, name):
    from tests.test_fsl_struct_write import _layout

    table = CASES[name]()
    if name == "list_sliced":
        # pylance panics writing a sliced fixed_size_list of structs (its own bug); the same rows, freshly built, compare.
        table = pa.table(table.to_pydict(), schema=table.schema)
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nanolance.write_table(table, str(ours))
    lance_mod.write_dataset(table, str(theirs), data_storage_version="2.2")
    assert (_layout(ours) == _layout(theirs)) != (name in LAYOUT_DIFFERS)
