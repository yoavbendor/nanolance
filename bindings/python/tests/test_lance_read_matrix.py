"""Reading what stock Lance writes, across types and row counts.

The write direction is well covered: `test_write_encoding_matrix.py` walks 21 shapes x 3 modes and
checks that both readers agree. The *read* direction had only spot tests, and that is the direction
every stock-Lance bug in this project has been found in -- because the encoding is not ours to
choose. Lance picks it from the data, and **the choice depends on the row count**: the same column
comes back flat at 100 rows, dictionary-encoded at 1024, and RLE'd at 5000.

So this walks types x sizes and compares against pylance's own read of the same dataset. It is the
counterpart to the write matrix, and it found four failures the day it was written -- none of which
any existing test touched.

The known-failing cells are asserted to fail **with their specific message** rather than skipped. A
skip rots quietly; an assertion that a gap still exists fails the moment someone closes it, which is
exactly when this file should be revisited.
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


# Each entry generates one column of n rows. The shapes are chosen to land on different Lance
# encodings -- runs, low cardinality, high cardinality, constants -- not merely different types.
KINDS = {
    "int64_wide": lambda n: pa.array(list(range(n)), pa.int64()),
    "int32_narrow": lambda n: pa.array([i % 100 for i in range(n)], pa.int32()),
    "int16": lambda n: pa.array([i % 30000 for i in range(n)], pa.int16()),
    "uint8": lambda n: pa.array([i % 250 for i in range(n)], pa.uint8()),
    "int64_constant": lambda n: pa.array([7] * n, pa.int64()),
    "float32": lambda n: pa.array([i * 0.5 for i in range(n)], pa.float32()),
    "float64": lambda n: pa.array([i * 0.125 for i in range(n)], pa.float64()),
    "bool": lambda n: pa.array([i % 3 == 0 for i in range(n)], pa.bool_()),
    "str_runs": lambda n: pa.array([f"run-{i // 400}" for i in range(n)], pa.utf8()),
    "str_lowcard": lambda n: pa.array([f"row-{i % 50}" for i in range(n)], pa.utf8()),
    "str_highcard": lambda n: pa.array([f"v-{i}-{i * i}" for i in range(n)], pa.utf8()),
    "str_with_empties": lambda n: pa.array(["" if i % 3 else f"s{i}" for i in range(n)], pa.utf8()),
    "binary": lambda n: pa.array([bytes([i % 251]) * ((i % 5) + 1) for i in range(n)], pa.binary()),
    "fixed_size_binary": lambda n: pa.array([f"{i:08d}".encode() for i in range(n)], pa.binary(8)),
    "timestamp_us": lambda n: pa.array(
        [datetime.datetime(2026, 1, 1) + datetime.timedelta(seconds=i) for i in range(n)],
        pa.timestamp("us"),
    ),
    "date32": lambda n: pa.array(
        [datetime.date(2026, 1, 1) + datetime.timedelta(days=i % 3000) for i in range(n)], pa.date32()
    ),
    "time64_us": lambda n: pa.array(
        [datetime.time(i % 24, (i * 7) % 60) for i in range(n)], pa.time64("us")
    ),
    "decimal128": lambda n: pa.array(
        [decimal.Decimal(f"{i % 10000}.{i % 100:02d}") for i in range(n)], pa.decimal128(12, 2)
    ),
    "struct": lambda n: pa.array([{"a": i, "b": f"s{i % 7}"} for i in range(n)]),
}

# 1024/1025 straddle a FastLanes block. 5000 is where Lance starts run-length encoding a repetitive
# string column, and 20000 is where it starts building dictionaries for fixed-width types.
SIZES = (1, 100, 1024, 1025, 5000, 20_000)


# Known read gaps, each reproduced by this matrix and each a distinct missing decoder. Recorded as
# (predicate, message fragment); see docs/PROGRESS.md for the page descriptors behind them.
KNOWN_GAPS = (
    # A dictionary with RLE'd indices, where the dictionary itself is LZ4-compressed
    # (`Rle{Flat(32), Flat(8)}` over `General{LZ4, Variable}`). nanolance writes dict+RLE with a zstd
    # dictionary and reads that; this combination lands in the wrong branch and mis-sizes the buffer.
    (lambda n, k: k == "str_runs" and n >= 5000, "Expected string array buffer"),
)


def _expected_gap(n, kind):
    for predicate, message in KNOWN_GAPS:
        if predicate(n, kind):
            return message
    return None


@pytest.mark.parametrize("n", SIZES)
@pytest.mark.parametrize("kind", sorted(KINDS))
def test_stock_lance_column_reads_back(lance_mod, tmp_path, kind, n):
    path = str(tmp_path / f"{kind}_{n}.lance")
    table = pa.table({"c": KINDS[kind](n)})
    lance_mod.write_dataset(table, path)
    expected = lance_mod.dataset(path).to_table()

    gap = _expected_gap(n, kind)
    if gap is not None:
        with pytest.raises(Exception) as excinfo:
            pa.table(nanolance.read_table(path))
        assert gap in str(excinfo.value), (
            f"the known gap for {kind} at n={n} now fails differently: {excinfo.value}. "
            f"If it was fixed, delete its entry from KNOWN_GAPS."
        )
        return

    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()


def test_the_gap_list_is_not_stale(lance_mod, tmp_path):
    """Every KNOWN_GAPS entry must match at least one cell this matrix actually generates.

    Without this, narrowing SIZES or renaming a kind would silently orphan an entry, and the suite
    would keep advertising a gap nobody is reproducing any more.
    """
    for index, (predicate, message) in enumerate(KNOWN_GAPS):
        assert any(predicate(n, k) for n in SIZES for k in KINDS), (
            f"KNOWN_GAPS[{index}] ({message!r}) matches no cell in the matrix"
        )


# A dictionary whose entries are fixed-width values rather than a variable-width block. Lance builds
# one for a temporal or decimal column once the cardinality justifies it, and the block it writes has
# NO header -- it is the values end to end. The variable-width dictionary path read the first value as
# an offset header and refused with "dict block header invalid".
#
# The matrix above covers the non-null case at the two sizes where it first appears. These are the
# axes it does not cross: the nullable variant (definition levels in front of the indices), 16-byte
# and 32-byte entries, and enough rows for the dictionary to span several pages.
FIXED_WIDTH_DICTIONARY = {
    "time64": (pa.time64("us"), lambda i: datetime.time(i % 24, (i * 7) % 60)),
    "timestamp": (pa.timestamp("us"), lambda i: datetime.datetime(2026, 1, 1) + datetime.timedelta(seconds=i % 500)),
    "date32": (pa.date32(), lambda i: datetime.date(2026, 1, 1) + datetime.timedelta(days=i % 300)),
    "decimal128": (pa.decimal128(12, 2), lambda i: decimal.Decimal(f"{i % 10000}.{i % 100:02d}")),
    "decimal256": (pa.decimal256(40, 2), lambda i: decimal.Decimal(f"{i % 5000}.{i % 100:02d}")),
}


@pytest.mark.parametrize("n", (1024, 20_000, 65_536))
@pytest.mark.parametrize("nullable", (False, True), ids=("nonnull", "nullable"))
@pytest.mark.parametrize("name", sorted(FIXED_WIDTH_DICTIONARY))
def test_fixed_width_dictionary_reads_back(lance_mod, tmp_path, name, nullable, n):
    arrow_type, value = FIXED_WIDTH_DICTIONARY[name]
    values = [None if nullable and i % 11 == 0 else value(i) for i in range(n)]
    path = str(tmp_path / f"{name}_{n}_{nullable}.lance")
    lance_mod.write_dataset(pa.table({"c": pa.array(values, arrow_type)}), path)
    expected = lance_mod.dataset(path).to_table()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()
