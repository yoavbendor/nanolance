"""A constant column that also has nulls, read from a stock-Lance dataset.

This file exists because of a **silent** wrong answer, which this project treats as its worst
failure mode. A nullable constant column -- the same value in every row that is not null -- read back
with every row valid, or with every row null, depending on the type. No exception, no warning, just
different data than pylance returns from the same file.

Lance writes a constant page as `ConstantLayout`, and the layout is decided by two things together:
whether the descriptor carries an inline value, and how many buffers the page has.

    inline, 0 buffers  -> the value, no levels
    inline, 2 buffers  -> the value; buffer 0 = rep, buffer 1 = def
    no inline, 1       -> buffer 0 = the value, no levels
    no inline, 3       -> buffer 0 = the value; buffer 1 = rep, buffer 2 = def

`ConstantPageScheduler::try_new` refuses every other combination, and so do we.

Two separate mistakes came out of reading only half of that:

1. **A fixed-width nullable constant lost its nulls.** The definition buffer was skipped outright --
   the descriptor's `def_compression` and `num_def_values` fields were not even parsed. Every row
   came back as the constant value. An all-zero nullable `int64` column is the plain case.
2. **A variable-width nullable constant lost its values.** "All null" was inferred from "no inline
   value AND a definition layer", but a variable-width constant NEVER has an inline value -- it
   keeps its value in a buffer. So every nullable string constant read back entirely null. The real
   rule is the buffer count, and it is the one above.

The levels themselves are raw u16, one per row, always. `ConstantLayout.def_compression` exists in
the proto but applies only to the all-null path; a page carrying a value borrows the buffer as a u16
slice and nothing else. pylance leaves `num_def_values` at 0 on this path, so the row count is what
says how many levels there are -- another reason the descriptor alone could not answer this.
"""

from __future__ import annotations

import datetime
import decimal

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture
def lance_mod():
    return require_pylance()


# Fixed-width types carry their value inline; variable-width ones keep it in a buffer. Both spellings
# have to be covered, because they failed for opposite reasons and in opposite directions.
VALUES = {
    "int64": (pa.int64(), 7),
    "int32": (pa.int32(), 7),
    "bool": (pa.bool_(), True),
    "float64": (pa.float64(), 1.5),
    "fixed_size_binary": (pa.binary(4), b"abcd"),
    "timestamp": (pa.timestamp("us"), datetime.datetime(2026, 1, 1)),
    "decimal128": (pa.decimal128(12, 2), decimal.Decimal("1.25")),
    "string": (pa.utf8(), "same"),
    "binary": (pa.binary(), b"same"),
}

# `none` and `allnull` are the edges the buffer-count rule turns on: one produces a page with no
# definition layer, the other a page with no value at all.
PATTERNS = {
    "none": lambda i: False,
    "every7": lambda i: i % 7 == 0,
    "first_50": lambda i: i < 50,
    "one": lambda i: i == 3,
    "allnull": lambda i: True,
}

SIZES = (100, 5_000, 300_000)


@pytest.mark.parametrize("n", SIZES)
@pytest.mark.parametrize("pattern", sorted(PATTERNS))
@pytest.mark.parametrize("name", sorted(VALUES))
def test_constant_column_with_nulls_matches_pylance(lance_mod, tmp_path, name, pattern, n):
    arrow_type, value = VALUES[name]
    is_null = PATTERNS[pattern]
    table = pa.table({"c": pa.array([None if is_null(i) else value for i in range(n)], arrow_type)})
    path = str(tmp_path / f"{name}_{pattern}_{n}.lance")
    lance_mod.write_dataset(table, path)

    expected = lance_mod.dataset(path).to_table()
    got = pa.table(nanolance.read_table(path))
    assert got.to_pydict() == expected.to_pydict()


def test_the_null_count_itself_is_right(lance_mod, tmp_path):
    """Values and validity agreed in the whole-table comparison above; check the count directly too.

    A bitmap that is right row by row but carries a stale `null_count` is a wrong Arrow array that
    `to_pydict()` would not notice, because it reconstructs the list from the bitmap.
    """
    n = 5_000
    table = pa.table({"c": pa.array([None if i % 7 == 0 else 7 for i in range(n)], pa.int64())})
    path = str(tmp_path / "counted.lance")
    lance_mod.write_dataset(table, path)

    column = pa.table(nanolance.read_table(path)).column(0)
    assert column.null_count == table.column(0).null_count
    assert column.null_count == len([i for i in range(n) if i % 7 == 0])
