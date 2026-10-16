"""Expression value types, phase A1: decimal values in filters.

Each case runs the same filter on nanolance and on pylance 12.0.0 and compares the matching rows
(or that both refuse). Datasets are written by both writers from the same table; the scalar-index
path is compared by building a btree index on decimal128 columns and filtering with and without it.
"""

from __future__ import annotations

import random
from decimal import Decimal

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
