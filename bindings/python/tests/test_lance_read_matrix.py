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


# ── The value-size axis ──────────────────────────────────────────────────────────────────────────
#
# Row count and cardinality are not the only things Lance decides on. It picks a page's LAYOUT from
# the page's LONGEST value: below 256 bytes (`MINIBLOCK_MAX_BYTE_LENGTH_PER_VALUE` in lance-encoding)
# the page is a MiniBlock; at or above it, a FullZip. So one long string among thousands of short
# ones changes how the whole column is stored, and a float32 fixed_size_list crosses the line at 64
# dimensions -- which is every real embedding.
#
# Each shape maps to the message it fails with today, or None once it reads correctly. A shape that
# starts reading must be flipped to None here -- the assertion below fails loudly until it is.
N_WIDE = 2_000


def _one_long(n):
    values = [f"short-{i}" for i in range(n)]
    values[n // 2] = "y" * 300
    return pa.array(values)


def _fsl(dim, nullable=False):
    import random

    rng = random.Random(dim)
    flat = pa.array([rng.random() for _ in range(N_WIDE * dim)], pa.float32())
    mask = pa.array([i % 9 == 0 for i in range(N_WIDE)]) if nullable else None
    return pa.FixedSizeListArray.from_arrays(flat, dim, mask=mask)


def _fsl_item_nulls(dim):
    flat = pa.array([None if i % 13 == 0 else float(i % 1000) for i in range(N_WIDE * dim)], pa.float32())
    return pa.FixedSizeListArray.from_arrays(flat, dim)


def _fsl_null_rows_pyarrow(dim):
    return pa.array(
        [None if i % 7 == 0 else [float((i + j) % 100) for j in range(dim)] for i in range(N_WIDE)],
        pa.list_(pa.float32(), dim),
    )


VALUE_SIZE_SHAPES = {
    # (column builder, the failure it produces today or None)
    "str_255": (lambda: pa.array([("x" * 250) + f"{i:05d}" for i in range(N_WIDE)]), None),
    "str_256": (lambda: pa.array([("x" * 251) + f"{i:05d}" for i in range(N_WIDE)]), None),
    "str_one_long": (lambda: _one_long(N_WIDE), None),
    "str_1024": (lambda: pa.array([("w" * 1019) + f"{i:05d}" for i in range(N_WIDE)]), None),
    "str_long_nullable": (
        lambda: pa.array([None if i % 7 == 0 else ("z" * 400) + str(i) for i in range(N_WIDE)]),
        None,
    ),
    # Distinct values on purpose: a LOW-cardinality long binary column is dictionary-encoded first
    # (251 distinct 1 KiB values came back as a MiniBlock dictionary page), so the size rule only
    # decides the layout of what the dictionary step leaves alone.
    "binary_1024": (lambda: pa.array([i.to_bytes(4, "little") * 256 for i in range(N_WIDE)], pa.binary()), None),
    "fsl_8": (lambda: _fsl(8), None),
    "fsl_32": (lambda: _fsl(32), None),
    "fsl_64": (lambda: _fsl(64), None),
    "fsl_768": (lambda: _fsl(768), None),
    "fsl_768_nullable": (lambda: _fsl(768, nullable=True), None),
    # Nulls INSIDE the vectors (not whole null rows): Lance sets FixedSizeList.has_validity. On a
    # MiniBlock page that is a second chunk buffer of element bits; on a FullZip page, ceil(N/8) bytes
    # of bits at the head of every row's slot. Element 0 is null on purpose -- the first bit is the
    # one a lazily-built bitmap is most likely to drop.
    "fsl_item_nulls": (lambda: _fsl_item_nulls(4), None),
    "fsl_768_item_nulls": (lambda: _fsl_item_nulls(768), None),
    # What `pa.array([None, [..]], pa.list_(t, N))` builds: pyarrow marks every element of a null row
    # null too, so an ORDINARY nullable vector column is written with element validity.
    "fsl_4_null_rows_pyarrow": (lambda: _fsl_null_rows_pyarrow(4), None),
    "fsl_768_null_rows_pyarrow": (lambda: _fsl_null_rows_pyarrow(768), None),
}


@pytest.mark.parametrize("name", sorted(VALUE_SIZE_SHAPES))
def test_value_size_axis(lance_mod, tmp_path, name):
    build, gap = VALUE_SIZE_SHAPES[name]
    path = str(tmp_path / f"{name}.lance")
    lance_mod.write_dataset(pa.table({"c": build()}), path)
    expected = lance_mod.dataset(path).to_table()

    if gap is not None:
        with pytest.raises(Exception) as excinfo:
            pa.table(nanolance.read_table(path))
        assert gap in str(excinfo.value), (
            f"{name} now fails differently: {excinfo.value}. If it reads now, set its gap to None."
        )
        return
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()


# Row ranges and deletions over the same shapes. Both re-cut the decoded column row by row, and a
# fixed_size_list carries a SECOND bitmap -- N element bits per row -- that has to be cut with it.
# A range whose start is not a multiple of 8 rows puts the element bits at a non-byte offset.
RANGE_SHAPES = ("str_long_nullable", "fsl_768_nullable", "fsl_item_nulls", "fsl_768_item_nulls",
                "fsl_4_null_rows_pyarrow")


@pytest.mark.parametrize("name", RANGE_SHAPES)
def test_value_size_shapes_under_ranges_and_deletions(lance_mod, tmp_path, name):
    build, _ = VALUE_SIZE_SHAPES[name]
    column = build()
    table = pa.table({"id": pa.array(range(len(column)), pa.int64()), "c": column})
    path = str(tmp_path / f"{name}.lance")
    lance_mod.write_dataset(table, path)
    expected = lance_mod.dataset(path).to_table()
    for offset, length in ((0, 1), (3, 17), (N_WIDE // 3, N_WIDE // 3), (N_WIDE - 5, 5)):
        got = pa.table(nanolance.read_table(path, offset=offset, length=length))
        got.validate(full=True)
        assert got.to_pydict() == expected.slice(offset, length).to_pydict(), (offset, length)

    lance_mod.dataset(path).delete("id % 5 == 1 OR id < 3")
    expected = lance_mod.dataset(path).to_table()
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == expected.to_pydict()
    got = pa.table(nanolance.read_table(path, offset=11, length=40))
    assert got.to_pydict() == expected.slice(11, 40).to_pydict()
