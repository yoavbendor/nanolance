"""Filters of the shape `(integer column op literal) cmp literal` -- `id % 100 = 0`, `x / 3 > 10`, `5 - x = 0` --
evaluated column at a time (expr.cpp fast_arith_compare) as the per-row path evaluates them: int64, wrapping
+ - *, truncating / and %, null in null out. Every row set equals pylance's; a zero divisor is still an error."""

from __future__ import annotations

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


@pytest.fixture(scope="module")
def dataset(tmp_path_factory):
    rng = np.random.default_rng(0)
    n = 20_000

    def col(values, t):
        return pa.array(values, type=t, mask=rng.random(n) < 0.05)

    table = pa.table({"i64": col(rng.integers(-1000, 1000, n), pa.int64()),
                      "i32": col(rng.integers(-1000, 1000, n), pa.int32()),
                      "i8": col(rng.integers(-100, 100, n), pa.int8()),
                      "u32": col(rng.integers(0, 5000, n), pa.uint32())})
    path = str(tmp_path_factory.mktemp("arith") / "d.lance")
    nl.write_dataset(table, path)
    return path


EXPRESSIONS = ["{c} % 7 = 3", "{c} % 7 = -3", "{c} % 100 != 0", "{c} % -7 = 2", "{c} / 3 > 10", "{c} + 5 < 0",
               "{c} - 5 >= 2", "5 - {c} = 0", "{c} * 3 <= -30", "3 > {c} % 4", "{c} % 7 = 3 AND {c} > 0"]


@pytest.mark.parametrize("column", ["i64", "i32", "i8", "u32"])
@pytest.mark.parametrize("expression", EXPRESSIONS)
def test_rows_match_pylance(dataset, column, expression):
    lance = require_pylance()
    flt = expression.format(c=column)

    def rows(m):
        return sorted(m.dataset(dataset).to_table(filter=flt, columns=[column]).column(column).to_pylist(),
                      key=lambda x: (x is None, x))

    assert rows(nl) == rows(lance), flt


def test_zero_divisor_is_still_an_error(dataset):
    with pytest.raises(Exception, match="division by zero"):
        nl.dataset(dataset).to_table(filter="i64 % 0 = 1")
