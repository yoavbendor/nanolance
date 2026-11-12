"""Reads of fixed_size_list<fixed_size_list<T, a>, b> FullZip pages, written by pylance 12 for rows of 256 bytes or more.

pylance writes such a column as FullZip: each row's slot is the outer validity (one bit an inner list, when the outer
level has validity), then the leaf validity (one bit a leaf, when the leaves have it), then the leaves. nanolance read
these pages as "unsupported page layout" before; it now decodes the nullable ones. Each file is written by pylance and
read whole, by take and by row range, by pylance and by nanolance; every read equals the pyarrow input.

A pylance file of a column with no nulls at all is not read by pylance either (its descriptor says 2048 bits a row for
8 x 16 float32, which is 4096 bits of data: "failed to fill whole buffer"), so nanolance refuses that page by name
rather than guess at its layout.
"""

from __future__ import annotations

import random

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _table(rows, outer, inner, typ, seed):
    rng = random.Random(seed)
    values = []
    for _ in range(rows):
        if rng.random() < 0.1:
            values.append(None)
            continue
        row = []
        for _ in range(outer):
            if rng.random() < 0.1:
                row.append(None)
                continue
            row.append([None if rng.random() < 0.1 else (rng.random() * 100 if typ == pa.float32() else rng.randint(-50, 50))
                        for _ in range(inner)])
        values.append(row)
    return pa.table({"id": pa.array(range(rows), pa.int64()),
                     "v": pa.array(values, pa.list_(pa.list_(typ, inner), outer))})


@pytest.mark.parametrize("outer,inner,typ", [(8, 16, pa.float32()), (4, 8, pa.int64()), (3, 40, pa.float64())])
def test_pylance_fullzip_fsl_of_fsl_read_whole_take_and_range(tmp_path, lance_mod, outer, inner, typ):
    table = _table(2000, outer, inner, typ, seed=outer * 100 + inner)
    path = str(tmp_path / "pylance.lance")
    lance_mod.write_dataset(table, path, max_rows_per_file=1500)
    ours = nl.dataset(path)
    assert lance_mod.dataset(path).to_table().equals(table)
    assert ours.to_table().equals(table)
    rows = [0, 1, 999, 1000, 1999]
    assert ours.take(rows).equals(table.take(rows))
    assert ours.to_table(offset=700, limit=900).equals(table.slice(700, 900))


def test_pylance_fullzip_fsl_of_fsl_without_nulls_is_refused_by_name(tmp_path, lance_mod):
    """pylance's descriptor for this page says 2048 bits a row; the data is 4096. nanolance does not guess."""
    values = [[[float(i + j * 16 + k) for k in range(16)] for j in range(8)] for i in range(2000)]
    table = pa.table({"v": pa.array(values, pa.list_(pa.list_(pa.float32(), 16), 8))})
    path = str(tmp_path / "no_nulls.lance")
    lance_mod.write_dataset(table, path, max_rows_per_file=1500)
    with pytest.raises(Exception, match="unsupported page layout"):
        nl.dataset(path).to_table()
