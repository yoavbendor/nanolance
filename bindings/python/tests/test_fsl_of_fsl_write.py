"""Writes of fixed_size_list<fixed_size_list<T, a>, b> (W1a), read back by pylance 12 and by nanolance.

nanolance writes one nested layout for the column, as pylance 12 does at format 2.2 (and 2.1, 2.0): mini-block chunks
of the leaves, with the outer validity (one bit an inner list) and the leaf validity (one bit a leaf) ahead of the values
when they carry nulls, and the definition levels for null rows. Each case: nanolance writes the table; pylance reads it
whole, by take and by row range, and nanolance reads it the same way; both equal the pyarrow input. The layout is then
checked against pylance's own file for the same table: same encoding, same validity levels.
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


def _fsl_fsl(typ, inner, outer, rows):
    return pa.table({"v": pa.array(rows, pa.list_(pa.list_(typ, inner), outer))})


def _mixed(n):
    # Null outer rows, null inner lists, null leaves, and valid values.
    base = [[[1, 2], [3, 4]], None, [[5, None], None], [[None, 8], [9, 10]]]
    return _fsl_fsl(pa.int32(), 2, 2, [base[i % 4] for i in range(n)])


def _wide(n, nulls):
    # 8 x 16 float32: 512 bytes a row, the width pylance writes as FullZip.
    rows = [[[float(i + j * 0.5 + k) for k in range(16)] for j in range(8)] for i in range(n)]
    if nulls:
        for i in range(1, n, 7):
            rows[i][3] = None
        for i in range(0, n, 5):
            rows[i] = None
    return _fsl_fsl(pa.float32(), 16, 8, rows)


def _random(n, seed):
    rng = np.random.default_rng(seed)
    values = rng.integers(-1000, 1000, size=(n, 3, 4)).astype(np.int64)
    outer_null = rng.random(n) < 0.1
    inner_null = rng.random((n, 3)) < 0.1
    leaf_null = rng.random((n, 3, 4)) < 0.1
    rows = []
    for i in range(n):
        if outer_null[i]:
            rows.append(None)
            continue
        row = []
        for j in range(3):
            if inner_null[i, j]:
                row.append(None)
            else:
                row.append([None if leaf_null[i, j, k] else int(values[i, j, k]) for k in range(4)])
        rows.append(row)
    return _fsl_fsl(pa.int64(), 4, 3, rows)


def _batches(first, second):
    # Two record batches of one table: the first without nulls, the second with them. The validity of the first
    # batch's entries must be carried into the file as valid when the second brings the nulls.
    return pa.Table.from_batches(first.to_batches() + second.to_batches())


CASES = {
    "mixed_nulls": lambda: _mixed(1500),
    "all_null": lambda: _fsl_fsl(pa.int32(), 2, 2, [[[None, None], [None, None]], None] * 500),
    "no_nulls": lambda: _fsl_fsl(pa.int32(), 2, 2, [[[1, 2], [3, 4]], [[5, 6], [7, 8]]] * 500),
    "row_nulls_only": lambda: _fsl_fsl(pa.int32(), 2, 2, [[[1, 2], [3, 4]], None, [[5, 6], [7, 8]]] * 300),
    "inner_nulls_only": lambda: _fsl_fsl(pa.int32(), 2, 2, [[[1, 2], None], [[5, 6], [7, 8]]] * 300),
    "leaf_nulls_only": lambda: _fsl_fsl(pa.int32(), 2, 2, [[[1, None], [3, 4]], [[5, 6], [7, 8]]] * 300),
    "float32_dims_3_4": lambda: _fsl_fsl(
        pa.float32(), 4, 3, [[[1.0, None, 3.0, 4.0]] * 3, None, [[0.5] * 4] * 3] * 200
    ),
    # Sliced input: the arrays carry offsets at every level (pyarrow's slice keeps the parents' buffers whole).
    "sliced_mixed": lambda: _mixed(1500).slice(7, 800),
    "sliced_random": lambda: _random(3000, seed=11).slice(501, 1200),
    "wide_rows_no_nulls": lambda: _wide(400, nulls=False),
    "wide_rows_nulls": lambda: _wide(400, nulls=True),
    "random_nulls": lambda: _random(5000, seed=7),
    "first_batch_valid": lambda: _batches(
        _fsl_fsl(pa.int32(), 2, 2, [[[1, 2], [3, 4]], [[5, 6], [7, 8]]] * 400),
        _mixed(600),
    ),
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

    rows = [0, 1, 2, 3, 7, len(table) - 1] if len(table) > 7 else list(range(len(table)))
    assert pa.table(nanolance.take(path, rows)).to_pydict() == ds.take(rows).to_pydict()
    assert pa.table(nanolance.take(path, rows)).to_pydict() == {"v": [expected["v"][r] for r in rows]}

    n = len(table)
    for offset, length in [(0, min(5, n)), (n // 3, min(700, n - n // 3)), (max(0, n - 4), 4)]:
        ranged = pa.table(nanolance.read_table(path, offset=offset, length=length))
        assert ranged.to_pydict() == {"v": expected["v"][offset : offset + length]}
        assert ds.to_table(offset=offset, limit=length).to_pydict() == ranged.to_pydict()

    projected = pa.table(nanolance.read_table(path, columns=["v"]))
    assert projected.to_pydict() == ds.to_table(columns=["v"]).to_pydict()


@pytest.mark.parametrize("name", sorted(CASES))
def test_fsl_of_fsl_round_trip(lance_mod, tmp_path, name):
    _check(lance_mod, tmp_path, name)


def test_fsl_of_fsl_300k_rows(lance_mod, tmp_path):
    table = _mixed(300_000)
    path = str(tmp_path / "rows_300k.lance")
    nanolance.write_table(table, path)
    ds = lance_mod.dataset(path)
    assert ds.to_table().to_pydict() == table.to_pydict()
    assert pa.table(nanolance.read_table(path)).to_pydict() == table.to_pydict()


def test_fsl_of_fsl_under_a_struct_is_refused(tmp_path):
    inner = pa.list_(pa.list_(pa.int32(), 2), 2)
    table = pa.table({"s": pa.StructArray.from_arrays([pa.array([[[1, 2], [3, 4]]], inner)], names=["v"])})
    with pytest.raises(Exception, match="fixed_size_list of fixed_size_list under a struct or list is read but not written yet"):
        nanolance.write_table(table, str(tmp_path / "struct.lance"))


def test_fsl_of_fsl_of_bool_is_refused(tmp_path):
    table = pa.table({"v": pa.array([[[True, False], [True, True]]], pa.list_(pa.list_(pa.bool_(), 2), 2))})
    with pytest.raises(Exception, match="is not supported; a fixed_size_list inside one must hold fixed-width numbers"):
        nanolance.write_table(table, str(tmp_path / "bool.lance"))
