"""IN lists over unindexed columns: a long list is one hash lookup per row, and it must answer what SQL's IN answers.

The reference is SQL's three-valued IN written out in Python over the rows a plain scan returns (after deletes):
TRUE if an item equals the value; else NULL if the value or an item is NULL; else FALSE. NOT IN negates a non-NULL
result. A list of more than eight non-NULL items takes the hash path, so the lists are checked just under that
threshold (8 items) and just over it (9), with NULL items, duplicates, whole and fractional floats, and numbers out
of the column's range mixed in.
"""

from __future__ import annotations

import random

import pyarrow as pa
import pyarrow.compute as pc
import pytest

import nanolance.lance as nano

ROWS = 3000
INT_COLUMNS = {
    "i8": (pa.int8(), -128, 127),
    "i16": (pa.int16(), -32768, 32767),
    "i32": (pa.int32(), -(2**31), 2**31 - 1),
    "i64": (pa.int64(), -(2**63), 2**63 - 1),
    "u8": (pa.uint8(), 0, 255),
    "u16": (pa.uint16(), 0, 65535),
    "u32": (pa.uint32(), 0, 2**32 - 1),
    "u64": (pa.uint64(), 0, 2**64 - 1),
}
TEXT_VALUES = ["", "a", "b", "abc", "é", "日本語", "a b", "zz", "Ä", "ab"]
TEXT_COLUMNS = {"s": pa.string(), "lg": pa.large_string(), "bin": pa.binary(), "lbin": pa.large_binary()}


def _int_values(rng, lo, hi):
    edges = [lo, hi, 0, 1, -1]
    out = []
    for r in range(ROWS):
        if r % 17 == 0:
            out.append(None)
        elif r % 7 == 0:
            out.append(rng.choice([v for v in edges if lo <= v <= hi]))
        else:
            out.append(rng.randint(lo, hi))
    return out


@pytest.fixture(scope="module")
def dataset(tmp_path_factory):
    """Two fragments, then deletes, so a fragment carries a deletion vector; NULLs in every column."""
    rng = random.Random(42)
    columns = {"rid": pa.array(range(ROWS), pa.int64())}
    for name, (typ, lo, hi) in INT_COLUMNS.items():
        columns[name] = pa.array(_int_values(rng, lo, hi), typ)
    for name, typ in TEXT_COLUMNS.items():
        values = [rng.choice(TEXT_VALUES + [None]) for _ in range(ROWS)]
        if typ in (pa.binary(), pa.large_binary()):
            values = [None if v is None else v.encode() for v in values]
        columns[name] = pa.array(values, typ)
    table = pa.table(columns)
    path = str(tmp_path_factory.mktemp("in_lists") / "t.lance")
    nano.write_dataset(table.slice(0, ROWS // 2), path)
    nano.write_dataset(table.slice(ROWS // 2), path, mode="append")
    nano.dataset(path).delete("rid % 13 = 0 OR rid = 1500")
    return path


def _sql_in(value, items, negate):
    """SQL's IN over one value: True, False or None (NULL); NOT IN negates a non-NULL result."""
    if value is None:
        return None
    hit = False
    null_item = False
    for item in items:
        if item is None:
            null_item = True
        elif value == item:
            hit = True
            break
    result = True if hit else (None if null_item else False)
    if negate and result is not None:
        result = not result
    return result


def _literal(v):
    if v is None:
        return "NULL"
    if isinstance(v, str):
        return "'" + v + "'"
    if isinstance(v, float):
        return repr(v)
    return str(v)


def _items(column, n_items, rng):
    """`n_items` non-NULL items: values of the column (or text values) with duplicates, whole floats (2.0 is 2),
    fractions (2.5 is nothing), zeros (-0.0 is 0), numbers out of range, and one above int64."""
    if column not in INT_COLUMNS:
        return [rng.choice(TEXT_VALUES) for _ in range(n_items)]
    typ, lo, hi = INT_COLUMNS[column]
    items = []
    while len(items) < n_items:
        kind = rng.random()
        if kind < 0.45:
            items.append(rng.randint(lo, hi))
        elif kind < 0.55:
            items.append(float(rng.randint(lo, hi)))
        elif kind < 0.62:
            items.append(rng.randint(lo, hi) + 0.5)
        elif kind < 0.67:
            items.append(rng.choice([0.0, -0.0]))
        elif kind < 0.75:
            items.append(rng.choice([-1000000, 1000000]))
        elif kind < 0.78:
            items.append(2**63 + 5)
        else:
            items.append(items[-1] if items else lo)  # a duplicate
    return items


def _rows(path):
    return nano.dataset(path).to_table().to_pylist()


def _norm(v):
    """A binary value compares with the text literal the filter names: as text."""
    return v.decode("utf-8") if isinstance(v, bytes) else v


def _check(path, rows, column, items, negate):
    sql = f"{column} {'NOT IN' if negate else 'IN'} ({', '.join(_literal(i) for i in items)})"
    got = sorted(nano.dataset(path).to_table(columns=["rid"], filter=sql).column("rid").to_pylist())
    expected = sorted(
        r["rid"]
        for r in rows
        if _sql_in(_norm(r[column]), [_norm(i) for i in items], negate) is True
    )
    assert got == expected, sql


@pytest.mark.parametrize("column", list(INT_COLUMNS))
@pytest.mark.parametrize("negate", [False, True])
@pytest.mark.parametrize("n_items", [1, 8, 9, 40])
def test_integer_lists_match_sql_in(dataset, column, negate, n_items):
    rows = _rows(dataset)
    items = _items(column, n_items, random.Random(f"{column}/{n_items}/{negate}"))
    _check(dataset, rows, column, items + [None], negate)  # a NULL item: non-matches are NULL


@pytest.mark.parametrize("column", list(TEXT_COLUMNS))
@pytest.mark.parametrize("negate", [False, True])
@pytest.mark.parametrize("n_items", [1, 8, 9, 30])
def test_text_lists_match_sql_in(dataset, column, negate, n_items):
    rows = _rows(dataset)
    items = _items(column, n_items, random.Random(f"{column}/{n_items}/{negate}"))
    items += ["", "not-there", "日本語"]  # the empty string is a value of its own
    _check(dataset, rows, column, items + [None], negate)


def test_only_null_items_match_nothing(dataset):
    rows = _rows(dataset)
    for column in ("i32", "s"):
        _check(dataset, rows, column, [None] * 9, False)  # NULL for every row: no row passes
        _check(dataset, rows, column, [None] * 9, True)
    # A value among nine NULL items: its rows are TRUE, every other row is NULL.
    _check(dataset, rows, "i32", [None] * 9 + [5], False)
    _check(dataset, rows, "i32", [None] * 9 + [5], True)


def test_integer_lists_agree_with_pyarrow_is_in(dataset):
    """An independent reference: pyarrow.compute.is_in over the same values, with same-typed items."""
    table = nano.dataset(dataset).to_table()
    for column, (typ, _lo, _hi) in INT_COLUMNS.items():
        present = sorted({v for v in table.column(column).to_pylist() if v is not None})
        items = present[:: max(1, len(present) // 12)][:12]
        items = items + items[:3]  # with duplicates
        sql = f"{column} IN ({', '.join(str(v) for v in items)})"
        got = sorted(nano.dataset(dataset).to_table(columns=["rid"], filter=sql).column("rid").to_pylist())
        mask = pc.fill_null(pc.is_in(table.column(column), value_set=pa.array(items, typ)), False)
        expected = sorted(table.column("rid").filter(mask).to_pylist())
        assert got == expected, sql


def test_integers_against_doubles_above_2_53_compare_as_doubles(tmp_path):
    """compare_column compares an integer column with a double as doubles: above 2^53 two integers can round to
    the same double. A list with such an item takes the per-literal path, which keeps that rule: 2^53 and 2^53 + 1
    both equal 9007199254740992.0 (2^53 + 1 rounds to it), 2^53 + 2 does not."""
    big = 2**53
    values = [big, big + 1, big + 2, 5]
    path = str(tmp_path / "big.lance")
    nano.write_dataset(pa.table({"rid": pa.array(range(len(values)), pa.int64()),
                                 "v": pa.array(values, pa.int64())}), path)
    items = list(range(100, 108)) + [float(big)]  # nine items: the hash path is asked first
    sql = f"v IN ({', '.join(str(i) if isinstance(i, int) else repr(i) for i in items)})"
    got = sorted(nano.dataset(path).to_table(columns=["rid"], filter=sql).column("rid").to_pylist())
    expected = sorted(r for r, v in enumerate(values) if float(v) == float(big))
    assert got == expected == [0, 1], sql
