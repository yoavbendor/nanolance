"""A fixed_size_list whose items are all null, read from a stock-Lance dataset.

Lance 12 writes such a column with the items as a `Constant` CompressiveEncoding (field 3) inside a
FullZip page: no item bytes at all, only the element validity of each row. The constant carries no
value when the items are null, and a value when they are not. nanolance refused these pages by name
("unsupported FullZip value compression") until the Constant was modelled.

Each shape is written by pylance and must read back with nanolance and with pylance, equal.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture
def lance_mod():
    return require_pylance()


def _fsl_table(typ, dim, rows):
    return pa.table({"v": pa.array(rows, pa.list_(typ, dim))})


def _all_null_row(dim):
    return [None] * dim


# Every item type and width, with a row that is null and without: the null row is what makes Lance
# keep the list's own validity next to the items' validity.
SHAPES = {}
for _name, _typ in [("int8", pa.int8()), ("int64", pa.int64()), ("float64", pa.float64()), ("float32", pa.float32())]:
    for _dim in (1, 2, 16, 128):
        SHAPES[f"{_name}_dim{_dim}_null_rows"] = (
            lambda typ=_typ, dim=_dim: _fsl_table(typ, dim, [_all_null_row(dim), None, _all_null_row(dim)])
        )
        SHAPES[f"{_name}_dim{_dim}_no_null_rows"] = (
            lambda typ=_typ, dim=_dim: _fsl_table(typ, dim, [_all_null_row(dim)] * 3)
        )

# Several pages: 100k rows, with and without null rows.
SHAPES["int32_dim2_100k"] = lambda: _fsl_table(pa.int32(), 2, [[None, None]] * 100_000)
SHAPES["int32_dim2_100k_null_rows"] = lambda: _fsl_table(
    pa.int32(), 2, [None if i % 3 == 0 else [None, None] for i in range(100_000)]
)

# A column that is all null at the top level (not an FSL), and constant non-null lists.
SHAPES["int64_all_null"] = lambda: pa.table({"v": pa.array([None, None, None], pa.int64())})
SHAPES["int32_all_null_100k"] = lambda: pa.table({"v": pa.array([None] * 100_000, pa.int32())})
SHAPES["int32_dim2_constant"] = lambda: _fsl_table(pa.int32(), 2, [[1, 1]] * 1000)
SHAPES["int32_dim2_constant_item_null"] = lambda: _fsl_table(pa.int32(), 2, [[1, None]] * 1000)

# The same list inside a struct, and a list of lists.
_STRUCT = pa.struct([("a", pa.list_(pa.int32(), 2))])
SHAPES["struct_fsl_all_null"] = lambda: pa.table(
    {"s": pa.array([{"a": [None, None]}, None, {"a": [None, None]}], _STRUCT)}
)
SHAPES["struct_fsl_all_null_100k"] = lambda: pa.table({"s": pa.array([{"a": [None, None]}] * 100_000, _STRUCT)})
SHAPES["list_fsl_all_null"] = lambda: pa.table(
    {"v": pa.array([[[None, None]], [[None, None], [None, None]], None], pa.list_(pa.list_(pa.int32(), 2)))}
)

# Items that are strings, and a list of lists of lists.
SHAPES["string_dim2_all_null"] = lambda: _fsl_table(pa.string(), 2, [[None, None], None])
SHAPES["fsl_of_fsl_all_null"] = lambda: pa.table(
    {"v": pa.array([[[None, None], [None, None]], None], pa.list_(pa.list_(pa.int32(), 2), 2))}
)


# Shapes pylance writes as Constant that nanolance still refuses. Strict xfail: a fix turns them red.
_LEFT = {
    "string_dim2_all_null": "fsl<string> all null: string is an unsupported fixed_size_list element type",
}

_PARAMS = [
    pytest.param(name, marks=pytest.mark.xfail(strict=True, reason=_LEFT[name])) if name in _LEFT else name
    for name in sorted(SHAPES)
]


@pytest.mark.parametrize("name", _PARAMS)
def test_all_null_fixed_size_list_matches_pylance(lance_mod, tmp_path, name):
    table = SHAPES[name]()
    path = str(tmp_path / f"{name}.lance")
    lance_mod.write_dataset(table, path)

    expected = lance_mod.dataset(path).to_table()
    got = pa.table(nanolance.read_table(path))
    assert got.to_pydict() == expected.to_pydict()
