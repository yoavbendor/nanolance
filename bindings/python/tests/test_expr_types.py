"""Expression value types, phase A1: decimal values in filters.

Each case runs the same filter on nanolance and on pylance 12.0.0 and compares the matching rows
(or that both refuse). Datasets are written by both writers from the same table; the scalar-index
path is compared by building a btree index on decimal128 columns and filtering with and without it.
"""

from __future__ import annotations

import datetime as dt
import glob
import math
import os
import random
from decimal import Decimal

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nano
from tests.support import require_pylance

D = Decimal


def _table() -> pa.Table:
    return pa.table(
        {
            "i": pa.array(range(7), pa.int64()),
            "v": pa.array([D("2.55"), D("-2.55"), D("2.00"), None, D("0.01"), D("1234567.89"), D("2.5")],
                          pa.decimal128(10, 2)),
            "w": pa.array([D("2.55"), D("-0.5"), D("2.000"), None, D("0.125"), D("1.50"), D("-2.55")],
                          pa.decimal256(76, 3)),
        }
    )


def _datasets(lance, tmp_path, table, **kw):
    ours = nano.write_dataset(table, str(tmp_path / "nano.lance"), **kw)
    theirs = lance.write_dataset(table, str(tmp_path / "lance.lance"), **kw)
    return ours, theirs


def _rows(ds, flt, **kw):
    try:
        t = ds.to_table(filter=flt, columns=["i"], **kw)
    except Exception as exc:  # noqa: BLE001 -- the outcome is what is compared
        return ("error", str(exc))
    return ("rows", sorted(t.column("i").to_pylist()))


def _assert_same(ours, theirs, flt, **kw):
    mine = _rows(ours, flt, **kw)
    other = _rows(theirs, flt, **kw)
    if other[0] == "error":
        assert mine[0] == "error", f"pylance refuses {flt!r}, nanolance returned {mine}"
    else:
        assert mine == other, f"{flt!r}"


# ---------------------------------------------------------------------------------------------
# 1. Equal and range filters on decimal128 / decimal256, int literals, nulls, mixed scales

FILTERS = [
    "v = 2",
    "v > 1",
    "v <= -2",
    "v != 2",
    "v IS NULL",
    "v BETWEEN 0 AND 3",
    "v IN (2, 2)",
    "w = 2",
    "w > -1",
    "w >= arrow_cast(-2.55, 'Decimal256(76, 2)')",
    "w < 1",
    "w = arrow_cast(-0.5, 'Decimal256(76, 1)')",
    "v = arrow_cast(2.5, 'Decimal128(3, 1)')",
    "v = arrow_cast(2.50, 'Decimal128(10, 3)')",
    "v = arrow_cast(2.00, 'Decimal256(76, 2)')",
    "v = arrow_cast(2, 'Decimal128(10, 2)')",
    "v = arrow_cast(-2, 'Decimal128(10, 2)')",
    "w = arrow_cast(2.55, 'Decimal128(10, 2)')",
    "w > arrow_cast(1, 'Decimal128(5, 1)')",
    "v > 2.5 OR i = 0",
    "v = 2 AND i > 1",
]


@pytest.mark.parametrize("flt", FILTERS)
def test_decimal_filters_equal_pylance(tmp_path, flt):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _table())
    _assert_same(ours, theirs, flt)


def test_decimal_filters_with_index_equal_pylance(tmp_path):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _table())
    for ds in (ours, theirs):
        ds.create_scalar_index("v", index_type="BTREE")
    for flt in ["v = 2", "v > 1", "v <= -2", "v = arrow_cast(2.545, 'Decimal128(10, 2)')"]:
        _assert_same(ours, theirs, flt)
        assert _rows(ours, flt, use_scalar_index=False) == _rows(ours, flt)


def test_decimal_filters_across_fragments_equal_pylance(tmp_path):
    lance = require_pylance()
    rng = random.Random(7)
    values = [None if rng.random() < 0.05 else D(rng.randrange(-10**8, 10**8)).scaleb(-4) for _ in range(3000)]
    table = pa.table({"i": pa.array(range(len(values)), pa.int64()),
                      "v": pa.array(values, pa.decimal128(20, 4))})
    ours, theirs = _datasets(lance, tmp_path, table, max_rows_per_file=400)
    for flt in ["v > 1", "v = 2", "v <= -0.5",
                "v >= arrow_cast(0.12345, 'Decimal128(10, 5)')",
                "v = arrow_cast(1234.5678, 'Decimal256(76, 4)')"]:
        _assert_same(ours, theirs, flt)


# ---------------------------------------------------------------------------------------------
# 2. arrow_cast: exact folding from the literal's text, rounding, overflow, Float64


@pytest.mark.parametrize(
    "flt",
    [
        "v = arrow_cast(2.545, 'Decimal128(10, 2)')",   # rounds half away from zero to 2.55
        "v = arrow_cast(-2.545, 'Decimal128(10, 2)')",  # to -2.55
        "v = arrow_cast(-2.555, 'Decimal128(10, 2)')",  # to -2.56: no row
        "v = arrow_cast(2.5551, 'Decimal128(10, 2)')",  # to 2.56: no row
        "v = arrow_cast(1234567.891, 'Decimal128(9, 2)')",
        "w = arrow_cast(0.125, 'Decimal256(4, 3)')",
        "w = arrow_cast(0.1251, 'Decimal256(4, 3)')",
        "v = arrow_cast(1e3, 'Decimal128(10, 2)')",
        "v = arrow_cast(2.0000001, 'Float64')",          # compares as a double
        "v > arrow_cast(2.5, 'Float64')",
        "v = arrow_cast(2.55, 'Float64')",
        "w = arrow_cast(2.55, 'Float64')",
    ],
)
def test_arrow_cast_decimal_equal_pylance(tmp_path, flt):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _table())
    _assert_same(ours, theirs, flt)


def test_arrow_cast_rounding_is_exact_text_not_double(tmp_path):
    # 2.555 is 2.55499999... as a double: a double-based cast would round it down to 2.55.
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _table())
    assert _rows(ours, "v = arrow_cast(2.555, 'Decimal128(10, 2)')") == ("rows", [])
    assert _rows(theirs, "v = arrow_cast(2.555, 'Decimal128(10, 2)')") == ("rows", [])
    assert _rows(ours, "v = arrow_cast(2.545, 'Decimal128(10, 2)')") == ("rows", [0])


@pytest.mark.parametrize(
    "flt, message",
    [
        ("v = 2.5", "could not convert to literal of type 'Decimal128"),
        ("v > 1.5", "could not convert to literal of type 'Decimal128"),
        ("v = 2.00", "could not convert to literal of type 'Decimal128"),
        ("v IN (2, 2.5)", "could not convert to literal of type 'Decimal128"),
        ("w = 2.55", "could not convert to literal of type 'Decimal256"),
        ("w < 1.5", "could not convert to literal of type 'Decimal256"),
    ],
)
def test_bare_float_literal_against_decimal_errors(tmp_path, flt, message):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _table())
    mine = _rows(ours, flt)
    assert mine[0] == "error" and message in mine[1]
    assert _rows(theirs, flt)[0] == "error"


def test_arrow_cast_overflow_errors_like_pylance(tmp_path):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _table())
    for flt in ["v = arrow_cast(12345, 'Decimal128(3, 0)')", "v = arrow_cast(2.5, 'Decimal128(3, 3)')",
                "v = arrow_cast(1234567.89, 'Decimal128(8, 2)')"]:
        mine = _rows(ours, flt)
        assert mine[0] == "error" and "is too large to store in a Decimal128" in mine[1], flt
        assert _rows(theirs, flt)[0] == "error", flt


# ---------------------------------------------------------------------------------------------
# 3. Projections of bare decimal columns are unchanged


@pytest.mark.parametrize("columns", [{"r": "v"}, {"r": "w", "i": "i"}, {"r": "v", "x": "arrow_cast(v, 'Float64')"}],
                         ids=["decimal128", "decimal256", "float64_of_decimal"])
def test_bare_decimal_projection_equals_pylance(tmp_path, columns):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _table())
    mine = ours.to_table(columns=columns)
    other = theirs.to_table(columns=columns)
    assert mine.schema.types == other.schema.types
    assert mine.to_pydict() == other.to_pydict()


# ---------------------------------------------------------------------------------------------
# 4. Duration columns (EXPR_TYPES session A2): every unit, exact cross-unit compares, nulls,
#    refused number / string literals, arrow_cast(<int>, 'Duration(Unit)'), bare projections.
def _duration_table() -> pa.Table:
    return pa.table(
        {
            "i": pa.array(range(8), pa.int64()),
            "s": pa.array([1, 2, None, -3, 0, 1, 2, 9000000000], pa.duration("s")),
            "ms": pa.array([2000, 2001, 2500, None, 0, -2000, 2000, 1], pa.duration("ms")),
            "us": pa.array([2_000_000, 2_000_001, None, 3, 0, -1, 2000, 1], pa.duration("us")),
            "ns": pa.array([2_000_000_000, 2_000_000_001, 2_500_000_000, None, 0, -2_000_000_000, 2_000_000_000, 1],
                           pa.duration("ns")),
        }
    )


DURATION_FILTERS = [
    # each unit against its own unit, then against every other unit
    "ms = arrow_cast(2000, 'Duration(Millisecond)')",
    "ms = arrow_cast(2, 'Duration(Second)')",
    "ms = arrow_cast(2000000, 'Duration(Microsecond)')",
    "ms = arrow_cast(2000000000, 'Duration(Nanosecond)')",
    "us = arrow_cast(2, 'Duration(Millisecond)')",
    "us = arrow_cast(2000001, 'Duration(Microsecond)')",
    "ns = arrow_cast(2, 'Duration(Second)')",
    "ns >= arrow_cast(2500, 'Duration(Millisecond)')",
    "s = arrow_cast(1, 'Duration(Second)')",
    "s = arrow_cast(-3, 'Duration(Second)')",
    "s = arrow_cast(1000, 'Duration(Millisecond)')",
    "s >= arrow_cast(1, 'Duration(Millisecond)')",
    "s > arrow_cast(1500, 'Duration(Millisecond)')",
    # ranges and sets across units, nulls and negatives
    "ms > arrow_cast(1, 'Duration(Second)')",
    "ms <= arrow_cast(2, 'Duration(Second)')",
    "us < arrow_cast(0, 'Duration(Second)')",
    "ns < arrow_cast(-2, 'Duration(Second)')",
    "ns != arrow_cast(2, 'Duration(Second)')",
    "ms BETWEEN arrow_cast(1, 'Duration(Second)') AND arrow_cast(3, 'Duration(Second)')",
    "ms NOT BETWEEN arrow_cast(1, 'Duration(Second)') AND arrow_cast(3, 'Duration(Second)')",
    "ms IN (arrow_cast(2, 'Duration(Second)'), arrow_cast(2001, 'Duration(Millisecond)'))",
    "us NOT IN (arrow_cast(2, 'Duration(Second)'))",
    "ms IS NULL",
    "us IS NOT NULL",
    # the extreme values: 9e9 seconds is 9e18 nanoseconds, just inside int64, so a naive conversion of
    # the other side would overflow; the compare must stay exact
    "s = arrow_cast(9000000000000000000, 'Duration(Nanosecond)')",
    "s = arrow_cast(9000000000, 'Duration(Second)')",
    "s > arrow_cast(8999999999999999999, 'Duration(Nanosecond)')",
    "s < arrow_cast(9000000000000000001, 'Duration(Nanosecond)')",
]


@pytest.mark.parametrize("flt", DURATION_FILTERS)
def test_duration_filters_equal_pylance(tmp_path, flt):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _duration_table())
    _assert_same(ours, theirs, flt)


def test_duration_filters_across_fragments_equal_pylance(tmp_path):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _duration_table(), max_rows_per_file=3)
    for flt in ("ms = arrow_cast(2000, 'Duration(Millisecond)')", "ns > arrow_cast(1, 'Duration(Second)')"):
        _assert_same(ours, theirs, flt)


@pytest.mark.parametrize(
    "flt, message",
    [
        ("ms = 2", "Received literal Int64(2) and could not convert to literal of type 'Duration(Millisecond)'"),
        ("ms = 2.5", "Received literal Float64(2.5) and could not convert to literal of type 'Duration(Millisecond)'"),
        ("ms = 2.0", "Received literal Float64(2) and could not convert to literal of type 'Duration(Millisecond)'"),
        ("ms > 1.5", "Received literal Float64(1.5) and could not convert to literal of type 'Duration(Millisecond)'"),
        ("ms IN (2)", "Received literal Int64(2) and could not convert to literal of type 'Duration(Millisecond)'"),
        ("ms BETWEEN 1 AND 3",
         "Received literal Int64(1) and could not convert to literal of type 'Duration(Millisecond)'"),
        ("us = 2", "Received literal Int64(2) and could not convert to literal of type 'Duration(Microsecond)'"),
        ("ns = 2.5", "Received literal Float64(2.5) and could not convert to literal of type 'Duration(Nanosecond)'"),
        ("s = 2", "Received literal Int64(2) and could not convert to literal of type 'Duration(Second)'"),
        ("ms = '2'", "Received literal Utf8(\"2\") and could not convert to literal of type 'Duration(Millisecond)'"),
    ],
)
def test_duration_bare_literal_errors_like_pylance(tmp_path, flt, message):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _duration_table())
    mine = _rows(ours, flt)
    assert mine[0] == "error" and message in mine[1], mine
    assert _rows(theirs, flt)[0] == "error"


def test_duration_arrow_cast_of_float_is_refused(tmp_path):
    ours, _ = _datasets(require_pylance(), tmp_path, _duration_table())
    mine = _rows(ours, "ms = arrow_cast(2.5, 'Duration(Millisecond)')")
    assert mine[0] == "error" and "integer literal" in mine[1]


@pytest.mark.parametrize("columns", [{"d": "ms"}, {"s": "s", "us": "us"}, {"ns": "ns", "i": "i"}],
                         ids=["millisecond", "second_and_microsecond", "nanosecond"])
def test_bare_duration_projection_equals_pylance(tmp_path, columns):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _duration_table())
    mine = ours.to_table(columns=columns)
    other = theirs.to_table(columns=columns)
    assert mine.schema.types == other.schema.types
    assert [str(t) for t in mine.schema.types] == [str(t) for t in other.schema.types]
    assert mine.to_pydict() == other.to_pydict()


# ---------------------------------------------------------------------------------------------
# 4. Scalar indexes on decimal128 / decimal256 / duration / uuid (BTREE, BITMAP, ZONEMAP): index-on
#    equals index-off equals pylance, the index is used, and the index files equal pylance's.

INDEX_KINDS = ["BTREE", "BITMAP", "ZONEMAP"]
UUID_A = b"0123012301230123"
UUID_B = b"2345234523452345"
UUID_HEX_A = "0x30313233303132333031323330313233"  # UUID_A as a hex literal
UUID_HEX_B = "0x32333435323334353233343532333435"  # UUID_B


def _index_table() -> pa.Table:
    return pa.table(
        {
            "i": pa.array(range(9), pa.int64()),
            "d": pa.array([D("2.55"), D("-2.55"), D("2.00"), None, D("0.01"), D("2.00"), D("1234567.89"),
                           D("2.5"), D("-0.10")], pa.decimal128(10, 2)),
            "e": pa.array([D("2.55"), D("-0.5"), D("2.000"), None, D("0.125"), D("2.000"), D("1.50"),
                           D("-2.55"), D("0.125")], pa.decimal256(76, 3)),
            "t": pa.array([2000, -1, 2000, None, 5, 2000, 0, 7, 1], pa.duration("ms")),
            "u": pa.array([UUID_A, UUID_B, UUID_A, None, b"\x00" * 16, UUID_B, b"\xff" * 16, UUID_A, UUID_B],
                          pa.uuid()),
        }
    )


INDEX_FILTERS = {
    "d": [
        "d = 2",
        "d = arrow_cast(2.5, 'Decimal128(3, 1)')",
        "d = arrow_cast(2.50, 'Decimal256(76, 3)')",
        "d > 1",
        "d <= arrow_cast(-2.555, 'Decimal128(10, 3)')",
        "d >= arrow_cast(0.005, 'Decimal128(10, 3)')",  # between two scales: exact, not rounded
        "d < arrow_cast(2.001, 'Decimal128(10, 3)')",
        "d IN (2, arrow_cast(0.01, 'Decimal128(10, 2)'), arrow_cast(-9.5, 'Decimal128(3, 1)'))",
        "d IS NULL",
        "d = arrow_cast(2.555, 'Decimal128(10, 3)')",  # no row: the literal is not a value of the scale
    ],
    "e": [
        "e = arrow_cast(0.125, 'Decimal256(76, 3)')",
        "e = arrow_cast(0.1250, 'Decimal128(10, 3)')",
        "e = 2",
        "e > arrow_cast(1.5, 'Decimal128(3, 1)')",
        "e <= arrow_cast(-0.5, 'Decimal128(10, 2)')",
        "e < arrow_cast(-0.499, 'Decimal128(10, 3)')",
        "e IN (arrow_cast(2.55, 'Decimal128(10, 2)'), arrow_cast(1.5, 'Decimal128(10, 1)'))",
        "e IS NULL",
    ],
    "t": [
        "t = arrow_cast(2, 'Duration(Second)')",
        "t = arrow_cast(2000000, 'Duration(Microsecond)')",
        "t > arrow_cast(1500000, 'Duration(Microsecond)')",
        "t <= arrow_cast(0, 'Duration(Second)')",
        "t >= arrow_cast(0, 'Duration(Nanosecond)')",
        "t IN (arrow_cast(5, 'Duration(Millisecond)'), arrow_cast(1, 'Duration(Second)'))",
        "t IS NULL",
        "t = 2",  # a bare integer is refused: pylance has no duration literal for it
    ],
    "u": [
        f"u = arrow_cast({UUID_HEX_B}, 'FixedSizeBinary(16)')",
        f"u = {UUID_HEX_A}",
        f"u > arrow_cast({UUID_HEX_A}, 'FixedSizeBinary(16)')",
        f"u IN (arrow_cast({UUID_HEX_A}, 'FixedSizeBinary(16)'), arrow_cast(0x00000000000000000000000000000000, 'FixedSizeBinary(16)'))",
        "u IS NULL",
        f"u = arrow_cast({UUID_HEX_A}, 'FixedSizeBinary(2)')",  # a 2-byte width: refused, as pylance does
    ],
}


def _index_files(lance, path):
    """Each index file under `_indices/<uuid>/`, by file name, read by pylance's reader."""
    files = {}
    for f in glob.glob(os.path.join(str(path), "_indices", "*", "*")):
        if os.path.isfile(f):
            files[os.path.basename(f)] = lance.file.LanceFileReader(f).read_all().to_table()
    return files


@pytest.mark.parametrize("kind", INDEX_KINDS)
@pytest.mark.parametrize("column", list(INDEX_FILTERS))
def test_index_filters_equal_pylance(tmp_path, kind, column):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _index_table(), max_rows_per_file=4)
    for ds in (ours, theirs):
        ds.create_scalar_index(column, index_type=kind)
    for flt in INDEX_FILTERS[column]:
        _assert_same(ours, theirs, flt)
        assert _rows(ours, flt, use_scalar_index=False) == _rows(ours, flt), flt


@pytest.mark.parametrize("kind", INDEX_KINDS)
@pytest.mark.parametrize("column", list(INDEX_FILTERS))
def test_index_is_used_for_comparisons_and_in(tmp_path, kind, column):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _index_table(), max_rows_per_file=4)
    ours.create_scalar_index(column, index_type=kind)
    for flt in INDEX_FILTERS[column]:
        if not any(op in flt for op in (" = ", " < ", " > ", " <= ", " >= ", " IN ")):
            continue
        if _rows(theirs, flt)[0] == "error":
            continue
        assert "ScalarIndexQuery" in ours.scanner(filter=flt).explain_plan(), flt


@pytest.mark.parametrize("kind", INDEX_KINDS)
def test_index_files_equal_pylance(tmp_path, kind):
    lance = require_pylance()
    for column in INDEX_FILTERS:
        root = tmp_path / column
        root.mkdir()
        ours, theirs = _datasets(lance, root, _index_table(), max_rows_per_file=4)
        ours.create_scalar_index(column, index_type=kind)
        theirs.create_scalar_index(column, index_type=kind)
        mine = _index_files(lance, root / "nano.lance")
        other = _index_files(lance, root / "lance.lance")
        assert sorted(mine) == sorted(other), column
        for name in other:
            assert mine[name].schema == other[name].schema, (column, name, mine[name].schema, other[name].schema)
            assert mine[name].to_pylist() == other[name].to_pylist(), (column, name)


def test_uuid_index_keeps_the_extension_type(tmp_path):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _index_table())
    ours.create_scalar_index("u", index_type="BTREE")
    flt = INDEX_FILTERS["u"][0]
    mine = ours.to_table(filter=flt, columns=["u"])
    other = theirs.to_table(filter=flt, columns=["u"])
    assert mine.schema == other.schema
    assert mine.column("u").to_pylist() == other.column("u").to_pylist()


# ---------------------------------------------------------------------------------------------
# Phase B, item 5: bracket indexing, `l[1]` (1-based, negative from the end) and `s['x']`.


def _bracket_table() -> pa.Table:
    return pa.table(
        {
            "i": pa.array(range(6), pa.int64()),
            "l": pa.array([[10, 20, 30], [], None, [None, 5], [7], [1, 2, 3, 4]], pa.list_(pa.int64())),
            "ls": pa.array(
                [[{"x": 1, "y": True}], [], None, [{"x": None, "y": False}, {"x": 9, "y": True}],
                 [{"x": 3, "y": None}], None],
                pa.list_(pa.struct([("x", pa.int64()), ("y", pa.bool_())])),
            ),
            "fl": pa.array([[1.5, 2.25, -3.0], None, [0.1, 1e6, 70000.0], [], [None, 2.0], [1.0]],
                           pa.list_(pa.float32())),
            "s": pa.array([{"x": 1}, None, {"x": None}, {"x": 4}, {"x": 5}, {"x": 6}],
                          pa.struct([("x", pa.int64())])),
        }
    )


def _proj(ds, expr):
    try:
        t = ds.to_table(columns={"r": expr})
    except Exception as exc:  # noqa: BLE001 -- the outcome is what is compared
        return ("error", str(exc))
    return ("values", t.column("r").to_pylist(), str(t.schema.field("r").type))


@pytest.mark.parametrize(
    "expr",
    ["l[1]", "l[2]", "l[3]", "l[0]", "l[-1]", "l[-4]", "l[4]", "l[9]", "l[-9]", "l[1] + 1",
     "CAST(l[1] AS DOUBLE)", "fl[-1]", "fl[2]", "ls[1].x", "ls[1]['y']",
     "ls[2]['x']", "ls[4]['x']", "ls[1]['x'] + 1", "s['x']", "s.x", "s['x'] + 1"],
)
def test_bracket_projection_equals_pylance(tmp_path, expr):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _bracket_table())
    mine = _proj(ours, expr)
    other = _proj(theirs, expr)
    assert other[0] == "values", other
    assert mine == other, expr


@pytest.mark.parametrize(
    "flt",
    ["l[1] = 10", "l[2] IS NULL", "l[1] IS NULL", "l[3] = 5", "l[-1] = 3", "l[0] IS NULL", "l[1] IN (10, 20)",
     "l[1] BETWEEN 5 AND 25", "ls[1]['x'] = 1", "ls[2]['x'] IS NULL", "s['x'] = 4", "s['x'] > 1 AND l[1] > 5",
     "fl[1] > 2", "ls[1]['y'] AND i > 0"],
)
def test_bracket_filter_equals_pylance(tmp_path, flt):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _bracket_table())
    _assert_same(ours, theirs, flt)
    assert _rows(theirs, flt)[0] == "rows", flt


@pytest.mark.parametrize("expr", ["l[1]['x']", "s[1]", "ls[1][1]", "l['1']", "l[1.0]"])
def test_bracket_errors_like_pylance(tmp_path, expr):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _bracket_table())
    assert _proj(theirs, expr)[0] == "error", expr
    assert _proj(ours, expr)[0] == "error", expr


# ---------------------------------------------------------------------------------------------
# Phase B, item 6: `_cast_list_f16(vector)`, the float16 cast pylance's vector search uses.


def _f16_table() -> pa.Table:
    return pa.table(
        {
            "i": pa.array(range(4), pa.int64()),
            "fsl": pa.array([[1.5, 2.25, -3.0], None, [0.1, 1e6, 70000.0], [2**-24, 1e-7, 65520.0]],
                            pa.list_(pa.float32(), 3)),
            "vl": pa.array([[1.5, 2.25], None, [0.1, 1e6], []], pa.list_(pa.float32())),
        }
    )


@pytest.mark.parametrize("column", ["fsl", "vl"])
def test_cast_list_f16_equals_pylance(tmp_path, column):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _f16_table())
    expr = f"_cast_list_f16({column})"
    mine = _proj(ours, expr)
    other = _proj(theirs, expr)
    assert other[0] == "values" and "halffloat" in other[2]
    assert mine == other


def test_cast_list_f16_rounds_like_numpy(tmp_path):
    lance = require_pylance()
    rng = np.random.default_rng(7)
    values = np.concatenate([
        rng.standard_normal(3000).astype(np.float32),
        (rng.standard_normal(2000) * 1e-5).astype(np.float32),  # subnormal in float16
        (rng.standard_normal(1000) * 3e4).astype(np.float32),  # near the overflow edge
        np.array([65504, 65520, -65520, 2.0**-24, 2.0**-25, 6.1035156e-05, np.inf], np.float32),
    ])
    table = pa.table({"i": pa.array(range(len(values)), pa.int64()),
                      "v": pa.array([[float(x)] for x in values], pa.list_(pa.float32()))})
    ours, theirs = _datasets(lance, tmp_path, table)
    with np.errstate(over="ignore"):
        expected = values.astype(np.float16).astype(np.float64)
    got = np.array([row[0] for row in ours.to_table(columns={"r": "_cast_list_f16(v)"})["r"].to_pylist()])
    assert np.array_equal(got, expected)
    assert _proj(ours, "_cast_list_f16(v)") == _proj(theirs, "_cast_list_f16(v)")


@pytest.mark.parametrize("expr", ["_cast_list_f16(i)", "_cast_list_f16(v)"])
def test_cast_list_f16_refuses_non_float_lists_like_pylance(tmp_path, expr):
    lance = require_pylance()
    table = pa.table({"i": pa.array(range(2), pa.int64()), "v": pa.array([[1, 2], None], pa.list_(pa.int64()))})
    ours, theirs = _datasets(lance, tmp_path, table)
    assert _proj(theirs, expr)[0] == "error", expr
    assert _proj(ours, expr)[0] == "error", expr


# ---------------------------------------------------------------------------------------------
# Phase B, item 3 (the A3 follow-up): decimals of different widths whose common type needs more
# than 76 digits are refused, as DataFusion refuses them. The rule: Decimal128 against Decimal256,
# and max(p1 - s1, p2 - s2) + max(s1, s2) > 76.


def _clash_table() -> pa.Table:
    return pa.table(
        {
            "i": pa.array(range(3), pa.int64()),
            "a": pa.array([D("1.500"), D("-2.000"), None], pa.decimal256(76, 3)),
            "b": pa.array([D("1.500"), D("0.000"), None], pa.decimal128(38, 3)),
            "c": pa.array([D("1.50000"), D("-2.00000"), None], pa.decimal128(10, 5)),
            "n": pa.array([D("1.5000"), D("0.0000"), None], pa.decimal128(10, 4)),
            "k": pa.array([D("1"), D("2"), None], pa.decimal256(76, 0)),
            "m": pa.array([D("1.5"), D("2.0"), None], pa.decimal128(38, 1)),
            "j": pa.array([D("1.5"), D("2.0"), None], pa.decimal256(40, 30)),
        }
    )


@pytest.mark.parametrize(
    "flt, message",
    [
        ("a = c", "Cannot infer common argument type for comparison operation Decimal256(76, 3) = Decimal128(10, 5)"),
        ("c = a", "Cannot infer common argument type for comparison operation Decimal128(10, 5) = Decimal256(76, 3)"),
        ("a < c", "Cannot infer common argument type for comparison operation Decimal256(76, 3) < Decimal128(10, 5)"),
        ("a <> n", "Cannot infer common argument type for comparison operation Decimal256(76, 3) != Decimal128(10, 4)"),
        ("k = m", "Cannot infer common argument type for comparison operation Decimal256(76, 0) = Decimal128(38, 1)"),
        ("a = arrow_cast(1, 'Decimal128(10, 4)')",
         "Cannot infer common argument type for comparison operation Decimal256(76, 3) = Decimal128(10, 4)"),
        ("a IN (c)", "Can not find compatible types to compare Decimal256(76, 3) with [Decimal128(10, 5)]"),
        ("a BETWEEN b AND c", "Failed to coerce types Decimal256(76, 3) and Decimal128(10, 5) in BETWEEN expression."),
    ],
)
def test_decimal_clash_is_refused_like_pylance(tmp_path, flt, message):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _clash_table())
    mine = _rows(ours, flt)
    assert mine[0] == "error" and message in mine[1], mine
    assert _rows(theirs, flt)[0] == "error"


@pytest.mark.parametrize(
    "flt",
    ["a = b", "k = j", "c = n", "a = arrow_cast(1.5, 'Decimal256(40, 3)')", "a = k", "a BETWEEN b AND b"],
)
def test_decimal_pairs_that_fit_are_not_refused(tmp_path, flt):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _clash_table())
    _assert_same(ours, theirs, flt)


# ---------------------------------------------------------------------------------------------
# 5. Result types and values of integer and float projections (EXPR_TYPES phase C1). Each
#    projection is typed as DataFusion types it: the same schema type and values as pylance, or
#    both refuse and nanolance's message is part of pylance's. The rows are the integer and float
#    rows of the C0 table (docs/tasks/data/expr_types_c0_table.txt) plus the cases the table lacks.
#    Filters keep their own semantics (section 5b).
C1_PROJECTIONS = [
    "i8 + 3", "i8 - 3", "i8 * 3",
    "i8 / 3", "i8 % 3", "i16 + 3",
    "i16 - 3", "i16 * 3", "i16 / 3",
    "i16 % 3", "i32 + 3", "i32 - 3",
    "i32 * 3", "i32 / 3", "i32 % 3",
    "i64 + 3", "i64 - 3", "i64 * 3",
    "i64 / 3", "i64 % 3", "u8 + 3",
    "u8 - 3", "u8 * 3", "u8 / 3",
    "u8 % 3", "u16 + 3", "u16 - 3",
    "u16 * 3", "u16 / 3", "u16 % 3",
    "u32 + 3", "u32 - 3", "u32 * 3",
    "u32 / 3", "u32 % 3", "u64 + 3",
    "u64 - 3", "u64 * 3", "u64 / 3",
    "u64 % 3", "i8 + 1.5", "i8 * 1.5",
    "i32 + 1.5", "i32 * 1.5", "i64 + 1.5",
    "i64 * 1.5", "u8 + 1.5", "u8 * 1.5",
    "u32 + 1.5", "u32 * 1.5", "i8 + i8",
    "i8 + i16", "i8 + i32", "i8 + i64",
    "i16 + i8", "i32 + i64", "u8 + i8",
    "u8 + i32", "u8 + u8", "u8 + u32",
    "u32 + i32", "u8 * i8", "u8 - u8",
    "u32 - u8", "i64 * u64", "u64 + i8",
    "i32 + f32", "f32 * f64", "f32 + 1",
    "f32 * 2.0", "f64 + i8", "f32 + 1.5",
    "i8 * f32", "f32 / 0", "i32 + 1.0",
    "u8 + 0.5", "i8 * 100", "i8 + 100",
    "i32 * 2000000000", "i64 * 9223372036854775807", "i8 / 0",
    "i32 % 0", "u8 - 10", "u32 - 5",
    "i8 / 2", "-7 / 2", "i32 / i32",
    "-i8", "-i16", "-i32",
    "-i64", "-u8", "-u32",
    "-f32", "abs(i8)", "abs(i16)",
    "abs(i32)", "abs(i64)", "abs(u8)",
    "abs(f32)", "abs(f64)", "abs(-3)",
    "coalesce(i32, 0)", "coalesce(i8, 0)", "coalesce(i8, 1000)",
    "coalesce(i32, 1.5)", "coalesce(i64, 2)", "coalesce(i32, i64)",
    "coalesce(f32, 1.5)", "coalesce(u8, 300)", "i8 + 1000",
    "i8 - 128", "i8 + 127", "u8 + 300",
    "u8 - 300", "u8 + -5", "i8 + -129",
    "i32 + 3000000000", "i64 + 3000000000", "u32 + 4294967296",
    "u8 * 1000", "i8 / -1", "i8 % -1",
    "i8 * -1", "i64 / 0", "i64 + f32",
    "u32 + f32", "u8 + f32", "i64 + f64",
    "u64 + i64", "u64 + i32", "u16 + i8",
    "u32 + i8", "i64 * u32", "u64 + u8",
    "i8 + u64", "u16 + i16", "f32 + 0.1",
    "f32 * 1e40", "f32 + 1e40", "f32 + 3000000000",
    "f32 + i64", "f32 + 3.0000001", "f32 * 2",
    "f32 + 1000000000000", "coalesce(f32, 1)", "coalesce(f32, f64)",
    "coalesce(i64, 1.5)", "coalesce(u8, -1)", "coalesce(i8, u8)",
    "coalesce(i8, 1000, i16)", "coalesce(i32, i8)", "-i8 + 1",
    "abs(i8) + 1", "abs(u32)", "-f64",
    "abs(i8 + 1)", "u8 + -1", "u64 + -1",
    "i8 % 0", "i64 / -1", "f32 % 0",
    "i8 + i8 + 1000", "coalesce(i8,1000,i16)", "(i8 + 1) * 2",
    "i8 + 1 + 1", "-(i8 + 1)",
]

# Known divergence, phase C3: arrow_cast to an integer type is parsed as CAST(... AS bigint).
C1_ARROW_CAST = [
    "i8 + arrow_cast(1, 'Int32')",
    "i8 + arrow_cast(1, 'UInt8')",
]

# Filters keep the rows nanolance returned before phase C1 (a bare float literal or an
# out-of-range literal is not refused in a filter; see EXPR_TYPES.md, reviewer's decision 1).
C1_FILTER_ROWS = [
    ("i8 + 100 > 0", [0, 2]),
    ("i8 + 100 = -56", []),
    ("i16 > 40000", []),
    ("i32 = 7.0", [2]),
    ("i8 + 1.5 > 0", [0, 2]),
    ("u8 - 10 > 100", [0]),
    ("i8 * 100 = 16", []),
    ("f32 + 0.1 > 1.6", [2]),
    ("f32 * 1e40 > 0", [0, 2]),
    ("i8 + i16 > 0", [0, 2]),
    ("u8 + u8 = 144", []),
    ("-i8 + 1 = -99", [0]),
    ("i8 + arrow_cast(1, 'Int32') = 101", [0]),
    ("coalesce(i8, 1000) = 1000", [3]),
    ("i32 + f32 > 0", [0, 2]),
    ("u64 - 3 > 1", [0, 2]),
    ("i64 * 1.5 > 7", [0, 2]),
    ("i8 / 0 = 1", "division by zero"),
    ("u8 + 300 > 0", [0, 1, 2]),
    ("f32 + 3.0000001 > 4.5", [0, 2]),
    ("abs(i8 + 1) > 100", [0, 1]),
    ("i8 - 3 < 0", [1]),
]


def _numeric_table() -> pa.Table:
    return pa.table(
        {
            "i": pa.array(range(4), pa.int64()),
            "i8": pa.array([100, -128, 7, None], pa.int8()),
            "i16": pa.array([30000, -5, 7, None], pa.int16()),
            "i32": pa.array([2000000000, -7, 7, None], pa.int32()),
            "i64": pa.array([5, -7, 7, None], pa.int64()),
            "u8": pa.array([200, 1, 7, None], pa.uint8()),
            "u16": pa.array([60000, 1, 7, None], pa.uint16()),
            "u32": pa.array([4000000000, 1, 7, None], pa.uint32()),
            "u64": pa.array([5, 1, 7, None], pa.uint64()),
            "f32": pa.array([1.5, -2.25, 3.0, None], pa.float32()),
            "f64": pa.array([0.5, 2.5, -1.25, None], pa.float64()),
        }
    )


def _numeric_projection(ds, expr):
    try:
        t = ds.to_table(columns={"r": expr})
    except Exception as exc:  # noqa: BLE001 -- the outcome is what is compared
        return ("error", str(exc))
    return ("values", str(t.schema.field("r").type), t.column("r").to_pylist())


def _same_values(a, b):
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if isinstance(x, float) and isinstance(y, float) and math.isnan(x) and math.isnan(y):
            continue
        if x != y:
            return False
    return True


@pytest.mark.parametrize("expr", C1_PROJECTIONS)
def test_numeric_projection_equals_pylance(tmp_path, expr):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _numeric_table())
    mine = _numeric_projection(ours, expr)
    other = _numeric_projection(theirs, expr)
    if other[0] == "error":
        assert mine[0] == "error", f"pylance refuses {expr!r} ({other[1]}); nanolance returned {mine}"
        assert mine[1] in other[1], f"{expr!r}: {mine[1]!r} is not in pylance's {other[1]!r}"
    else:
        assert mine[0] == "values", f"pylance returns {other[1]} for {expr!r}; nanolance refuses: {mine[1]}"
        assert mine[1] == other[1], f"{expr!r}: type {mine[1]} against pylance's {other[1]}"
        assert _same_values(mine[2], other[2]), f"{expr!r}: {mine[2]} against pylance's {other[2]}"


@pytest.mark.parametrize("expr", C1_ARROW_CAST)
def test_numeric_projection_arrow_cast_equals_pylance(tmp_path, expr):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _numeric_table())
    mine = _numeric_projection(ours, expr)
    other = _numeric_projection(theirs, expr)
    assert mine[0] == "values" and mine[1] == other[1] and _same_values(mine[2], other[2])


@pytest.mark.parametrize("flt, rows", C1_FILTER_ROWS)
def test_numeric_filters_keep_their_rows(tmp_path, flt, rows):
    ours = nano.write_dataset(_numeric_table(), str(tmp_path / "nano.lance"))
    got = _rows(ours, flt)
    if isinstance(rows, str):
        assert got[0] == "error" and rows in got[1], f"{flt!r}: {got}"
    else:
        assert got == ("rows", rows), f"{flt!r}"


# ── phase C2: decimal arithmetic in projections (exact, as pylance 12 types and values them) ──────


def _decimal_table() -> pa.Table:
    return pa.table(
        {
            "i": pa.array(range(4), pa.int64()),
            "dec": pa.array([D("12.34"), D("-0.05"), D("1.00"), None], pa.decimal128(10, 2)),
            "a": pa.array([D("12"), D("-7"), D("1"), None], pa.decimal128(10, 0)),
            "b": pa.array([D("1.2345"), D("-0.0005"), D("2.5"), None], pa.decimal128(10, 4)),
            "c": pa.array([D("1.5"), D("-3.25"), D("0.0000000001"), None], pa.decimal128(38, 10)),
            "e": pa.array([D("12.34"), D("-0.05"), None, D("1.00")], pa.decimal128(20, 2)),
            "f": pa.array([D("1.234"), D("-0.001"), D("9.999"), None], pa.decimal128(5, 3)),
            "h": pa.array([D("1.00"), None, D("3.33"), D("-2.50")], pa.decimal128(38, 2)),
            "lo": pa.array([D("1.5"), D("-0.25"), D("0.5"), None], pa.decimal128(38, 37)),
            "x": pa.array([D(9) * D(10) ** 37, D(5) * D(10) ** 37, D(1), None], pa.decimal128(38, 0)),
            "z": pa.array([D(10) ** 19, D(-10) ** 19, D(3), None], pa.decimal128(38, 0)),
            "fl": pa.array([0.5, 2.5, -1.25, None], pa.float64()),
            "u": pa.array([10, 7, 0, None], pa.uint64()),
            "s": pa.array([3, -2, 5, None], pa.int8()),
            "i8": pa.array([100, -128, 7, None], pa.int8()),
            "i32": pa.array([2000000000, -7, 7, None], pa.int32()),
        }
    )


# Projections that pylance returns: the same type and the same values, nulls and negatives included.
C2_PROJECTIONS = [
    "dec + 1", "dec * 2", "dec / 2", "dec / 3", "dec % 3", "dec + i32", "dec + i8", "dec - i8", "dec * u",
    "dec + u", "dec * dec", "dec + dec", "dec - dec", "dec * dec * dec", "dec * dec * 2", "-dec * 2", "-dec + 1",
    "abs(dec)", "-dec", "abs(-dec * 3)", "dec + fl", "dec / dec", "dec + 2", "dec + i", "dec * i", "dec * 0", "dec * arrow_cast(2, 'Decimal128(3, 0)')",
    "dec / arrow_cast(4, 'Decimal128(10, 4)')", "dec + lo", "h % 3", "h / 3", "h + h", "h * 2",
    # division scales (s1 + 4, precision p1 + s2 + 4, capped at 38) and the cap itself
    "a / a", "a / b", "b / a", "b / b", "c / c", "c / a", "e / f", "f / e", "e / e", "a / 3", "3 / a", "e / 3",
    "3 / e", "e / 100000", "i / a", "i / e",
    "lo / a", "lo + lo", "z * z", "x + z",
    # % and the scale of a remainder
    "e % f", "e % a", "e % 3", "u % s", "u % dec", "e % u",
    # u64 with a signed int is decimal(21, 0) / (38, 0) / (24, 4) / (20, 0), as pylance
    "u + i8", "u - s", "u * s", "s * u", "u / s", "u / a", "u + e", "u % a",
    # coalesce: the common type of its arguments (integers by digits, float64 as decimal(30, 15))
    "coalesce(dec, 1)", "coalesce(dec, 1.5)", "coalesce(e, a)", "coalesce(e, f)", "coalesce(a, e)",
    "coalesce(dec, fl)", "coalesce(fl, dec)", "coalesce(dec, -1.5)",
]

# Refused by both, with pylance's message contained in ours (the wording of the error may add a prefix).
C2_REFUSALS = [
    ("dec / 0", "Divide by zero error"),
    ("dec % 0", "Divide by zero error"),
    ("a / i", "Divide by zero error"),
    ("e / lo", "Cannot get result type for decimal operation Decimal128(20, 2) / Decimal128(38, 37): Arithmetic "
               "overflow: Overflow happened on: 10 ^ 41"),
    ("e % 0", "Divide by zero error"),
    ("dec + 1.5", "Received literal Float64(1.5) and could not convert to literal of type 'Decimal128(10, 2)'"),
    ("dec * 0.5", "Received literal Float64(0.5) and could not convert to literal of type 'Decimal128(10, 2)'"),
    ("dec / 100000000000", "Received literal Int64(100000000000) and could not convert to literal of type "
                           "'Decimal128(10, 2)'"),
    ("lo * 2", "Output scale of Decimal128(38, 37) * Decimal128(38, 37) would exceed max scale of 38"),
    ("x + x", "Arithmetic overflow: Overflow happened on: 90000000000000000000000000000000000000 + "
              "90000000000000000000000000000000000000"),
    ("x * z", "Arithmetic overflow: Overflow happened on: 90000000000000000000000000000000000000 * "
              "10000000000000000000"),
    ("x / z", "Arithmetic overflow: Overflow happened on: 90000000000000000000000000000000000000 * 10000"),
    ("lo / lo", "Arithmetic overflow: Overflow happened on: 15000000000000000000000000000000000000 * "
                "100000000000000000000000000000000000000"),
    ("lo / 2", "Arithmetic overflow: Overflow happened on: 15000000000000000000000000000000000000 * "
               "100000000000000000000000000000000000000"),
    ("x + lo", "Arithmetic overflow: Overflow happened on: 90000000000000000000000000000000000000 * "
               "10000000000000000000000000000000000000"),
]


def _decimal_projection(ds, expr):
    try:
        t = ds.to_table(columns={"r": expr})
    except Exception as exc:  # noqa: BLE001 -- the outcome is what is compared
        return ("error", str(exc))
    return ("values", str(t.schema.field("r").type), t.column("r").to_pylist())


@pytest.mark.parametrize("expr", C2_PROJECTIONS)
def test_decimal_projection_equals_pylance(tmp_path, expr):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _decimal_table())
    mine = _decimal_projection(ours, expr)
    other = _decimal_projection(theirs, expr)
    assert other[0] == "values", f"pylance refuses {expr!r}: {other[1]}"
    assert mine[0] == "values", f"pylance returns {other[1]} for {expr!r}; nanolance refuses: {mine[1]}"
    assert mine[1] == other[1], f"{expr!r}: type {mine[1]} against pylance's {other[1]}"
    assert _same_values(mine[2], other[2]), f"{expr!r}: {mine[2]} against pylance's {other[2]}"


@pytest.mark.parametrize("expr, message", C2_REFUSALS)
def test_decimal_projection_refusals_equal_pylance(tmp_path, expr, message):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _decimal_table())
    mine = _decimal_projection(ours, expr)
    other = _decimal_projection(theirs, expr)
    assert other[0] == "error" and message in other[1], f"pylance says {other}"
    assert mine[0] == "error" and message in mine[1], f"{expr!r}: nanolance {mine} against {other[1]!r}"


@pytest.mark.parametrize("expr", ["z * z", "x + z", "c / a"])
def test_decimal_results_past_their_precision_are_returned_like_pylance(tmp_path, expr):
    # pylance checks the 128-bit range only: a value with more digits than its precision is returned
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _decimal_table())
    assert _decimal_projection(ours, expr) == _decimal_projection(theirs, expr)


# Lance's resolve_expr types a literal by a column only at the top (`col op lit`, and every literal of `col op (..)`);
# every other literal keeps DataFusion's Int64, and an integer expression beside a decimal is decimal(p, 0) for its type.
LITERAL_RESOLUTION = [
    "dec + i32 * 1", "dec + i32 * 2", "dec + i8 * 2", "dec + u * 2", "dec + 1 * 2", "dec + i32 * i32",
    "dec + (i8 + i8)", "dec + (1 + 1)", "dec + (dec + 1)", "(dec + 1) * 2", "2 * (dec + 1)", "(dec + 1) + i32",
    "i32 + (dec + 1)", "(i32 + 1) + dec", "dec * 2 + i32 * 2", "(i8 + 1) * 300",
    # integers alone: the literals of `col op (..)` take the column's type; a u64 with a signed int is decimal at any depth
    "i8 + (1 + 1)", "i8 + (2 * 100)", "(i8 + 1) + 1", "1 + (i8 + 1)", "i32 + (1 - 2) * 3", "i8 * (u + 1)",
    "u + (i8 + 1)", "(u + i8) * 2", "(u + 1) + i8", "-(u + i8)", "abs(u + i8)", "dec + (u + i8)",
]


@pytest.mark.parametrize("expr", LITERAL_RESOLUTION)
def test_decimal_with_nested_integer_arithmetic_equals_pylance(tmp_path, expr):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _decimal_table())
    assert _decimal_projection(ours, expr) == _decimal_projection(theirs, expr)


@pytest.mark.parametrize(
    "flt, outcome",
    [
        ("dec + 1 > 13", "cannot apply '+' to decimal and integer"),
        ("dec * 2 = 24.68", "cannot apply '*' to decimal and integer"),
        ("dec - dec = 0", "cannot apply '-' to decimal and decimal"),
        ("-dec > 0", "cannot negate a decimal"),
        ("abs(dec) > 1", "abs() needs a number"),
    ],
)
def test_decimal_arithmetic_filters_are_unchanged(tmp_path, flt, outcome):
    # filters keep their semantics (C1): decimal arithmetic in a filter is refused as it was before C2
    ours = nano.write_dataset(_decimal_table(), str(tmp_path / "nano.lance"))
    got = _rows(ours, flt)
    assert got[0] == "error" and outcome in got[1], f"{flt!r}: {got}"


@pytest.mark.parametrize("flt, rows", [("u + 1 > 3", [0, 1]), ("i8 + 1 > 0", [0, 2])])
def test_integer_arithmetic_filters_keep_their_rows(tmp_path, flt, rows):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _decimal_table())
    assert _rows(ours, flt) == ("rows", rows)
    assert _rows(theirs, flt) == ("rows", rows)


# ── phase C3: integer and decimal casts, temporal arithmetic, in projections (and filters kept as they were) ──

def _c3_table() -> pa.Table:
    return pa.table(
        {
            "i": pa.array(range(4), pa.int64()),
            "i8": pa.array([100, -128, 7, None], pa.int8()),
            "i32": pa.array([2000000000, -7, 7, None], pa.int32()),
            "i64": pa.array([5_000_000_000, -7, 7, None], pa.int64()),
            "u8": pa.array([200, 1, 7, None], pa.uint8()),
            "f32": pa.array([1.5, -2.25, 3.0, None], pa.float32()),
            "f64": pa.array([2.345, -2.5, 3.9, None], pa.float64()),
            "dec": pa.array([D("12.34"), D("-0.05"), D("1.00"), None], pa.decimal128(10, 2)),
            "dur": pa.array([2000, 2001, None, 0], pa.duration("ms")),
            "dur2": pa.array([2, -1, 3, None], pa.duration("ms")),
            "durs": pa.array([1, 2, None, 0], pa.duration("s")),
            "durus": pa.array([5, 6, 7, None], pa.duration("us")),
            "durns": pa.array([1, 2, None, 0], pa.duration("ns")),
            "ts": pa.array([dt.datetime(2020, 1, 2, 3, 4, 5, 2000), None, dt.datetime(2000, 1, 1, 0, 0, 1),
                            dt.datetime(2021, 1, 1)], pa.timestamp("us")),
            "tsms": pa.array([dt.datetime(2020, 1, 2, 3, 4, 5), None, dt.datetime(2000, 1, 1),
                              dt.datetime(2021, 1, 1)], pa.timestamp("ms")),
            "tss": pa.array([dt.datetime(2020, 1, 2, 3, 4, 5), None, dt.datetime(2000, 1, 1),
                             dt.datetime(2021, 1, 1)], pa.timestamp("s")),
            "tsns": pa.array([dt.datetime(2020, 1, 2, 3, 4, 5, 2000), None, dt.datetime(2000, 1, 1, 0, 0, 1),
                              dt.datetime(2021, 1, 1)], pa.timestamp("ns")),
            "d32": pa.array([dt.date(2020, 1, 2), None, dt.date(2000, 1, 1), dt.date(2021, 1, 1)], pa.date32()),
            "d32b": pa.array([dt.date(2020, 1, 1), None, dt.date(2000, 1, 3), dt.date(2021, 1, 1)], pa.date32()),
        }
    )


C3_PROJECTIONS = [
    # integer arrow_cast keeps its target type in arithmetic
    "i8 + arrow_cast(1, 'Int32')", "i8 + arrow_cast(1, 'UInt8')", "i8 + arrow_cast(1, 'Int64')",
    "arrow_cast(i8, 'Int64') + i8", "arrow_cast(i8, 'Int32') + i8", "arrow_cast(u8, 'UInt16') - i8",
    "arrow_cast(f64, 'Int32')", "arrow_cast(f64, 'Int8')", "arrow_cast(dec, 'Int32')", "arrow_cast(dec, 'UInt8')",
    "arrow_cast(f32, 'Float64') + 1", "arrow_cast(dec, 'Float32')",
    # decimal casts
    "arrow_cast(dec, 'Decimal128(10,4)')", "arrow_cast(f32, 'Decimal128(10,4)')",
    "arrow_cast(f64, 'Decimal128(10,2)')", "arrow_cast(dec, 'Decimal128(3,1)')",
    "arrow_cast(dec, 'Decimal128(10,4)') / 2", "arrow_cast(dec, 'Decimal128(10,4)') * 2",
    "dec + arrow_cast(1, 'Int64')", "dec * arrow_cast(2, 'Decimal128(3, 0)')",
    "dec / arrow_cast(4, 'Decimal128(10, 4)')",
    # SQL CAST
    "CAST(dec AS INT)", "CAST(dec AS SMALLINT)", "CAST(dec AS TINYINT)", "CAST(dec AS BIGINT)",
    "CAST(dec AS FLOAT)", "CAST(dec AS DOUBLE)", "CAST(dec AS DECIMAL(10,4))", "CAST(dec AS DECIMAL(3,1))",
    "CAST(dec AS DECIMAL(2,0))", "CAST(i32 AS DECIMAL(12,2))", "CAST(f64 AS DECIMAL(10,2))",
    "CAST(f64 AS DECIMAL(10,0))", "CAST(f32 AS DECIMAL(10,2))", "CAST(i8 AS DECIMAL(3,0))",
    "CAST(i8 AS DECIMAL(3, 0)) + 1", "CAST(i8 AS TINYINT)", "CAST(f32 AS INT)", "CAST(u8 AS INT)",
    "CAST(dec + 1 AS INT)", "CAST(i8 + 1 AS INT)",
    # durations: same unit
    "dur + dur2", "dur - dur2", "dur2 - dur", "dur + dur", "dur - dur", "dur - dur - dur2", "durus + durus",
    "durus - durus", "coalesce(dur, dur2)",
    # timestamps
    "ts - ts", "ts - tsms", "tsms - ts", "tss - tsms", "tsms - tss", "tsns - tsms", "tsns - ts",
    "ts + dur", "ts - dur", "tsms + dur", "tsms - dur", "tsms + durs", "tss - durs", "ts + durs", "ts + durus",
    "dur + ts", "ts + arrow_cast(1, 'Duration(Second)')",
    # dates
    "d32 - d32", "d32 - d32b", "d32b - d32",
]

C3_REFUSALS = [
    ("arrow_cast(i32, 'Int8')", "Cast error: Can't cast value 2000000000 to type Int8"),
    ("arrow_cast(i64, 'Int32')", "Cast error: Can't cast value 5000000000 to type Int32"),
    ("arrow_cast(i8, 'UInt8')", "Cast error: Can't cast value -128 to type UInt8"),
    ("arrow_cast(u8, 'Int8')", "Cast error: Can't cast value 200 to type Int8"),
    ("CAST(i64 AS SMALLINT)", "Cast error: Can't cast value 5000000000 to type Int16"),
    ("CAST(u8 AS TINYINT)", "Cast error: Can't cast value 200 to type Int8"),
    ("arrow_cast(i32, 'Decimal128(10,4)')",
     "2000000000.0000 is too large to store in a Decimal128 of precision 10. Max is 999999.9999"),
    ("CAST(i32 AS DECIMAL(10,2))",
     "2000000000.00 is too large to store in a Decimal128 of precision 10. Max is 99999999.99"),
    ("CAST(i8 AS DECIMAL(2,0))", "100 is too large to store in a Decimal128 of precision 2. Max is 99"),
    ("d32 + dur", "Cannot coerce arithmetic expression Date32 + Duration(ms) to valid types"),
    ("d32 - dur", "Cannot coerce arithmetic expression Date32 - Duration(ms) to valid types"),
    ("dur + d32", "Cannot coerce arithmetic expression Duration(ms) + Date32 to valid types"),
    ("d32 + durs", "Cannot coerce arithmetic expression Date32 + Duration(s) to valid types"),
    ("abs(dur)", "Function 'abs' expects Numeric but received Duration(ms)"),
    ("abs(durs)", "Function 'abs' expects Numeric but received Duration(s)"),
    ("-dur", "Negation only supports numeric, interval and timestamp types"),
    ("-durns", "Negation only supports numeric, interval and timestamp types"),
]

C3_MIXED_UNIT = ["dur + durs", "dur - durus", "durs + dur", "dur + durus", "ts - ts - dur"]


@pytest.mark.parametrize("expr", C3_PROJECTIONS)
def test_c3_projection_equals_pylance(tmp_path, expr):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _c3_table())
    mine = _decimal_projection(ours, expr)
    other = _decimal_projection(theirs, expr)
    assert other[0] == "values", f"pylance refuses {expr!r}: {other[1]}"
    assert mine[0] == "values", f"pylance returns {other[1]} for {expr!r}; nanolance refuses: {mine[1]}"
    assert mine[1] == other[1], f"{expr!r}: type {mine[1]} against pylance's {other[1]}"
    assert _same_values(mine[2], other[2]), f"{expr!r}: {mine[2]} against pylance's {other[2]}"


@pytest.mark.parametrize("expr, message", C3_REFUSALS)
def test_c3_refusals_equal_pylance(tmp_path, expr, message):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _c3_table())
    mine = _decimal_projection(ours, expr)
    other = _decimal_projection(theirs, expr)
    assert other[0] == "error" and message in other[1], f"pylance says {other}"
    assert mine[0] == "error" and message in mine[1], f"{expr!r}: nanolance {mine} against {other[1]!r}"


@pytest.mark.parametrize("expr", C3_MIXED_UNIT)
def test_c3_mixed_unit_duration_arithmetic_is_refused(tmp_path, expr):
    # pylance returns a month_day_nano_interval here; nanolance refuses (EXPR_TYPES decision 3)
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _c3_table())
    other = _decimal_projection(theirs, expr)
    assert other[0] == "values" and other[1] == "month_day_nano_interval", other
    mine = _decimal_projection(ours, expr)
    assert mine[0] == "error" and "mixed-unit" in mine[1], mine


@pytest.mark.parametrize(
    "flt, outcome",
    [
        ("dur + dur2 > dur", "cannot apply '+' to duration and duration"),
        ("CAST(dec AS INT) = 12", "cannot CAST 12.34 AS int"),
        ("arrow_cast(dec, 'Decimal128(10,4)') > 1", "arrow_cast to 'Decimal128(10,4)' takes a number literal"),
        ("ts - tsms > dur", "cannot apply '-' to timestamp and timestamp"),
    ],
)
def test_c3_filters_keep_their_refusals(tmp_path, flt, outcome):
    # filters keep their semantics: these were refused before C3 and still are
    ours = nano.write_dataset(_c3_table(), str(tmp_path / "nano.lance"))
    got = _rows(ours, flt)
    assert got[0] == "error" and outcome in got[1], f"{flt!r}: {got}"


@pytest.mark.parametrize("flt", ["i8 + arrow_cast(1, 'Int32') > 0", "i8 + arrow_cast(1, 'UInt8') < 0",
                                 "CAST(i8 AS DECIMAL(3, 0)) > 0", "i8 + arrow_cast(1, 'Int64') > 0"])
def test_c3_filter_rows_unchanged(tmp_path, flt):
    lance = require_pylance()
    ours, theirs = _datasets(lance, tmp_path, _c3_table())
    mine = _rows(ours, flt)
    other = _rows(theirs, flt)
    assert other[0] == "rows" and mine == other, f"{flt!r}: nanolance {mine} against {other}"

