"""Reads of fixed_size_list<fixed_size_list<T, a>, b> that pylance 12 writes (W1a).

Each case is written by pylance, read back with nanolance's ``read_table``, ``take`` and a projection,
and must equal pylance's own read. Lance stores the outer list's element validity (b bits a row) and
the inner list's element validity (a*b bits a row) ahead of the values in a mini-block chunk; a
FullZip page with Constant items carries only the two validity levels per row.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _fsl_fsl(typ, inner, outer, rows):
    return pa.table({"v": pa.array(rows, pa.list_(pa.list_(typ, inner), outer))})


def _mixed(n):
    # Null outer rows, null outer items, null inner items, and valid values.
    base = [[[1, 2], [3, 4]], None, [[5, None], None]]
    return _fsl_fsl(pa.int32(), 2, 2, [base[i % 3] for i in range(n)])


CASES = {
    "mixed_nulls": (lambda: _mixed(1500), "mixed_nulls"),
    "all_null": (lambda: _fsl_fsl(pa.int32(), 2, 2, [[[None, None], [None, None]], None] * 500), "all_null"),
    "no_nulls": (lambda: _fsl_fsl(pa.int32(), 2, 2, [[[1, 2], [3, 4]], [[5, 6], [7, 8]]] * 500), "no_nulls"),
    "float32_dims_3_4": (
        lambda: _fsl_fsl(pa.float32(), 4, 3, [[[1.0, None, 3.0, 4.0]] * 3, None, [[0.5] * 4] * 3] * 200),
        "float32_dims_3_4",
    ),
    "rows_300k": (lambda: _mixed(300_000), "rows_300k"),
}


def _check(lance_mod, tmp_path, name):
    table = CASES[name][0]()
    path = str(tmp_path / f"{name}.lance")
    lance_mod.write_dataset(table, path)
    ds = lance_mod.dataset(path)
    expected = ds.to_table()

    got = pa.table(nanolance.read_table(path))
    assert got.to_pydict() == expected.to_pydict()

    rows = [0, 1, 2, 7, len(table) - 1] if len(table) > 7 else list(range(len(table)))
    taken = pa.table(nanolance.take(path, rows))
    assert taken.to_pydict() == ds.take(rows).to_pydict()

    projected = pa.table(nanolance.read_table(path, columns=["v"]))
    assert projected.to_pydict() == expected.select(["v"]).to_pydict()


@pytest.mark.parametrize("name", sorted(CASES))
def test_fsl_of_fsl_matches_pylance(lance_mod, tmp_path, name):
    _check(lance_mod, tmp_path, name)
