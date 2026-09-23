"""Reading what stock Lance writes, across types and row counts.

The write direction is well covered: `test_write_encoding_matrix.py` walks 21 shapes x 3 modes and
checks that both readers agree. The *read* direction had only spot tests, and that is the direction
every stock-Lance bug in this project has been found in -- because the encoding is not ours to
choose. Lance picks it from the data, and **the choice depends on the row count**: the same column
comes back flat at 100 rows, dictionary-encoded at 1024, and RLE'd at 5000.

So this walks types x sizes and compares against pylance's own read of the same dataset. It is the
counterpart to the write matrix, and it found four failures the day it was written -- none of which
any existing test touched. All four are now fixed, and `KNOWN_GAPS` is empty.

A known-failing cell is asserted to fail **with its specific message** rather than skipped. A skip
rots quietly; an assertion that a gap still exists fails the moment someone closes it, which is
exactly when this file should be revisited. That is not hypothetical -- it is how each of the four
came off the list.

Row count is not the only axis the writer decides on. **Cardinality** is another, and the dedicated
tests below exist because of it: a dictionary block has four different shapes depending on how many
distinct values a column has and how wide they are, and two of them were found only by varying the
number of distinct values while holding the type fixed.
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
    # Runs, not just low cardinality: past 20000 rows Lance encodes this as RLE'd indices over a
    # FIXED-WIDTH dictionary, `Rle{Flat(32),Flat(8)}` over `General{LZ4,Flat(64)}`. The matrix had a
    # run-shaped string column but no run-shaped integer one, and they take different branches.
    "int64_runs": lambda n: pa.array([i // 400 for i in range(n)], pa.int64()),
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


# Known read gaps: (predicate, message fragment), each reproduced by this matrix.
#
# EMPTY, and that is the point of keeping it. The matrix opened with four entries -- structs,
# fixed-width dictionaries at two widths, and dict+RLE over an LZ4 dictionary -- and each one failed
# this file the moment it was fixed, because it is pinned by its specific message. A new gap goes
# here with the message it actually produces; it does not get an xfail or a skip.
KNOWN_GAPS = ()


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


# Dictionary-encoded indices that are themselves RUN-LENGTH encoded: `Rle{Flat(32), Flat(8)}` over a
# dictionary, which Lance picks for any column whose values come in runs. Both dictionary shapes
# occur -- `Variable` for strings, `Flat(N)` for integers, temporals and decimals -- and they produce
# different output kinds, so both are covered here.
#
# The branch this exercises used to read the page as ONE chunk with nanolance's own private framing.
# Lance writes the ordinary miniblock grammar with as many chunks as the page needs, so a page past
# 1024 values decoded short and failed downstream with a buffer-size mismatch naming nothing useful.
DICT_RLE = {
    "string": (pa.utf8(), lambda i: f"run-{i // 400}"),
    "int64": (pa.int64(), lambda i: i // 400),
    "time64": (pa.time64("us"), lambda i: datetime.time((i // 400) % 24, (i // 400) % 60)),
    "decimal128": (pa.decimal128(12, 2), lambda i: decimal.Decimal(f"{i // 400}.00")),
}


@pytest.mark.parametrize("n", (5000, 20_000, 65_536))
@pytest.mark.parametrize("nullable", (False, True), ids=("nonnull", "nullable"))
@pytest.mark.parametrize("name", sorted(DICT_RLE))
def test_dictionary_with_rle_indices_reads_back(lance_mod, tmp_path, name, nullable, n):
    arrow_type, value = DICT_RLE[name]
    values = [None if nullable and i % 11 == 0 else value(i) for i in range(n)]
    path = str(tmp_path / f"{name}_{n}_{nullable}.lance")
    lance_mod.write_dataset(pa.table({"c": pa.array(values, arrow_type)}), path)
    expected = lance_mod.dataset(path).to_table()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()


# A fixed-width dictionary whose entries are themselves FastLanes bit-packed, which Lance picks once
# they are narrow enough for packing to pay. It has TWO spellings and they differ only in where the
# bit width is written:
#
#   InlineBitpacking(64)        -- a width word at the head of each 1024-value block
#   Bitpacked{64, Flat(width)}  -- the width in the descriptor, no word in the buffer
#
# An int64 column with runs moves from one to the other as its dictionary grows: ~164 entries gets
# the inline spelling, ~3000 the out-of-line one. Both need a multi-block walk past 1024 entries,
# and the out-of-line tail has two legal encodings (packed-and-padded, or raw words) told apart by
# the buffer's total length alone.
#
# `distinct` is what selects the spelling, so it is the axis here -- the row count only has to be
# large enough to reach the dictionary at all.
@pytest.mark.parametrize(
    "n, distinct",
    (
        (65_536, 164),      # inline, one block
        (300_000, 3_000),   # out-of-line, three blocks
        (500_000, 10_000),  # out-of-line, ten blocks
    ),
)
@pytest.mark.parametrize("nullable", (False, True), ids=("nonnull", "nullable"))
@pytest.mark.parametrize("name", ("int64", "timestamp"))
def test_bitpacked_dictionary_reads_back(lance_mod, tmp_path, name, nullable, n, distinct):
    run = n // distinct
    if name == "int64":
        arrow_type, value = pa.int64(), lambda i: i // run
    else:
        arrow_type = pa.timestamp("us")
        value = lambda i: datetime.datetime(2026, 1, 1) + datetime.timedelta(seconds=i // run)
    values = [None if nullable and i % 11 == 0 else value(i) for i in range(n)]
    path = str(tmp_path / f"{name}_{n}_{nullable}.lance")
    lance_mod.write_dataset(pa.table({"c": pa.array(values, arrow_type)}), path)
    expected = lance_mod.dataset(path).to_table()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()
