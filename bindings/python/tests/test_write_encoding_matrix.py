"""Every encoding nanolance can choose, written and then read back by BOTH readers.

This file exists because of a specific failure. The structural-dictionary encoding declared the u16
miniblock chunk grammar; Lance v2.2 refuses any miniblock page that does, and it validates a file's
whole page table before decoding anything -- so one dictionary-encoded column made *every other
column in the same dataset* unreadable by stock Lance. It survived for months because no test wrote a
scattered low-cardinality string column, and the one smoke test that came close reported its failure
as a ctest SKIP.

Individual encodings do have their own tests. What was missing is the *matrix*: a single place that
writes one table of every shape the writer's heuristics can pick, under both compression modes, and
asserts that nanolance AND pylance read back exactly what went in. Anything new the writer learns to
emit belongs here, so the next encoding cannot slip through the same gap.

Two things each case asserts, deliberately:

  - **the whole table**, not a sample -- a shifted or truncated column is the failure mode that looks
    plausible;
  - **every column read on its own**, because the dictionary bug's signature was that reading an
    *unrelated* column failed. A full-table read alone would not have localized it.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance

# Big enough that the writer's heuristics actually fire (bitpacking works in 1024-value blocks, the
# dictionary and RLE scans need real runs) and that a page spans several chunks -- the multi-chunk
# chunk-meta layout is where the dictionary bug lived.
N = 20_000

# A null every 11th row: not a divisor of 1024, so nulls land at different positions within each
# FastLanes block rather than lining up at block boundaries.
def _null(i: int) -> bool:
    return i % 11 == 0


def _table(values, arrow_type=None):
    return pa.table({"c": pa.array(values, type=arrow_type)})


# (id, table, what the writer should pick for it). The third element is documentation, not an
# assertion -- the heuristics are free to change; what must not change is that both readers agree.
SHAPES = [
    ("int_bitpack", _table([i % 900 for i in range(N)], pa.int64()), "InlineBitpacking"),
    ("int_rle", _table([i // 500 for i in range(N)], pa.int64()), "RLE"),
    ("int_constant", _table([7] * N, pa.int64()), "ConstantLayout"),
    ("uint32_bitpack", _table([i % 300 for i in range(N)], pa.uint32()), "InlineBitpacking"),
    ("str_constant", _table(["same"] * N, pa.utf8()), "ConstantLayout"),
    ("str_dict_scattered", _table([f"row-{i % 500}" for i in range(N)], pa.utf8()), "dictionary"),
    ("str_dict_rle", _table([f"run-{i // 400}" for i in range(N)], pa.utf8()), "dictionary+RLE"),
    ("str_highcard", _table([f"v-{i}-{i * i}" for i in range(N)], pa.utf8()), "variable (zstd if asked)"),
    ("binary_highcard", _table([f"v-{i}".encode() for i in range(N)], pa.binary()), "variable"),
    ("float64", _table([i * 0.125 for i in range(N)], pa.float64()), "byte-stream-split+zstd if asked"),
    ("float32", _table([i * 0.5 for i in range(N)], pa.float32()), "byte-stream-split+zstd if asked"),
    ("bool_packed", _table([i % 3 == 0 for i in range(N)], pa.bool_()), "1-bit packing"),
    ("fixed_size_binary", _table([f"{i:08d}".encode() for i in range(N)], pa.binary(8)), "flat"),
    ("struct", _table([{"a": i, "b": f"s{i % 7}"} for i in range(N)]), "per-child encodings"),
    # Nullable variants: the definition-level layer sits in front of the value encoding, so each of
    # these is a different code path from its non-null twin, not the same one with a bitmap.
    ("null_int_bitpack", _table([None if _null(i) else i % 900 for i in range(N)], pa.int64()), "bitpack + def levels"),
    ("null_int_rle", _table([None if _null(i) else i // 500 for i in range(N)], pa.int64()), "RLE + def levels"),
    ("null_str_dict", _table([None if _null(i) else f"row-{i % 500}" for i in range(N)], pa.utf8()), "dictionary + def levels"),
    ("null_str_highcard", _table([None if _null(i) else f"v-{i}" for i in range(N)], pa.utf8()), "variable + def levels"),
    ("null_bool", _table([None if _null(i) else i % 3 == 0 for i in range(N)], pa.bool_()), "1-bit + def levels"),
    ("null_float64", _table([None if _null(i) else i * 0.125 for i in range(N)], pa.float64()), "bss + def levels"),
    ("null_fixed_size_binary", _table([None if _null(i) else f"{i:08d}".encode() for i in range(N)], pa.binary(8)), "flat + def levels"),
    # Constant AND nullable. This cell was missing from both matrices for as long as they existed:
    # every constant shape above has no nulls, and every nullable shape has more than one distinct
    # value, so nothing ever produced a page that was both. On the READ side that product held a
    # silent wrong answer in each direction -- see test_lance_constant_nulls.py. The write side turns
    # out to be correct; this pins that rather than leaving it unstated.
    ("null_int_constant", _table([None if _null(i) else 7 for i in range(N)], pa.int64()), "constant + def levels"),
    ("null_str_constant", _table([None if _null(i) else "same" for i in range(N)], pa.utf8()), "constant + def levels"),
    ("null_float_constant", _table([None if _null(i) else 1.5 for i in range(N)], pa.float64()), "constant + def levels"),
    # Arrow's null TYPE, not nulls in a typed column: no buffers at all, written as an all-null
    # ConstantLayout. Here mainly for the combined-table tests below, where a column with zero data
    # buffers sits beside every other encoding in the same file.
    ("null_type", pa.table({"c": pa.nulls(N)}), "all-null ConstantLayout"),
    # A constant fixed-width column is stored INLINE in the page descriptor, which Lance caps at 32
    # bytes. The writer used to inline anything, and wrote the value's length as a single byte -- so
    # a constant value of 128+ bytes produced a descriptor NEITHER reader could parse: a corrupt file.
    # 32 is the last width that may inline; 33 and 200 must take the flat path.
    ("fsb_constant_32", _table([b"k" * 32] * N, pa.binary(32)), "ConstantLayout, inline"),
    ("fsb_constant_33", _table([b"k" * 33] * N, pa.binary(33)), "flat: too wide to inline"),
    ("fsb_constant_200", _table([b"k" * 200] * N, pa.binary(200)), "flat: too wide to inline"),
    # Vectors. fixed_size_list is one physical column with a FixedSizeList wrapper around Flat; the
    # null-row variant is built the way pyarrow builds it, with every element of a null row null too.
    ("fsl_f32_8", _table([[float(i + j) for j in range(8)] for i in range(N)], pa.list_(pa.float32(), 8)), "FixedSizeList{8xFlat}"),
    ("null_fsl_f32_4", _table([None if _null(i) else [float(i)] * 4 for i in range(N)], pa.list_(pa.float32(), 4)), "FixedSizeList + def levels"),
    ("fsl_i64_2", _table([[i, -i] for i in range(N)], pa.list_(pa.int64(), 2)), "FixedSizeList{2xFlat}"),
    # Long strings: stock Lance switches to FullZip at 256 bytes; nanolance keeps MiniBlock, which is
    # legal at any value size. This pins that stock Lance reads it.
    ("str_long", _table([("q" * 300) + str(i) for i in range(N)], pa.utf8()), "variable, 300-byte values"),
]

MODES = [
    pytest.param({}, id="structural"),
    pytest.param({"compression": True}, id="compress"),
    # The "raw Lance output" path: no structural encodings at all. It is the fallback anyone hits
    # with --no-structural, and nothing else covered it against pylance.
    pytest.param({"structural_encoding": False}, id="no_structural"),
]


def _assert_both_readers_agree(path, table):
    """nanolance and pylance must each return exactly what was written -- whole table and per column.

    Per column matters: a page-table-level rejection (the dictionary bug) shows up as an *unrelated*
    column failing to read, which a whole-table assertion alone would report as a mystery.
    """
    lance = require_pylance()
    expected = table.to_pydict()

    assert pa.table(nanolance.read_table(path)).to_pydict() == expected, "nanolance re-read mismatch"

    dataset = lance.dataset(str(path))
    assert dataset.to_table().to_pydict() == expected, "stock Lance re-read mismatch"
    for name in table.column_names:
        got = dataset.to_table(columns=[name]).column(0).to_pylist()
        assert got == table.column(name).to_pylist(), f"stock Lance mis-read column {name!r} on its own"


@pytest.mark.parametrize("options", MODES)
@pytest.mark.parametrize("name, table, expected_encoding", SHAPES, ids=[s[0] for s in SHAPES])
def test_written_encoding_is_readable_by_both(name, table, expected_encoding, options, tmp_path):
    path = tmp_path / f"{name}.lance"
    nanolance.write_table(table, path, **options)
    _assert_both_readers_agree(path, table)


def test_one_dataset_holding_every_shape_at_once(tmp_path):
    """All of them side by side in ONE file.

    This is the arrangement the bug actually broke: the page table is validated as a whole, so a
    column with a bad page layout takes its neighbours down with it. Writing each shape to its own
    dataset (above) cannot catch that, and neither can reading only the column under test.
    """
    combined = pa.table({name: table.column("c") for name, table, _ in SHAPES})

    path = tmp_path / "everything.lance"
    nanolance.write_table(combined, path, compression=True)
    _assert_both_readers_agree(path, combined)


@pytest.mark.parametrize("options", MODES)
def test_every_shape_survives_sliced_multi_batch_writes(options, tmp_path):
    """The same shapes fed as SLICES across several fragments.

    `Table.to_batches()` returns views into one buffer, addressed through `ArrowArray::offset`. The
    variable-width ingest path ignored that offset and silently re-ingested the first batch's values
    from row `chunksize` on. Encoding choice and batch slicing are independent axes, and the bug
    lived in their product -- so the matrix has to cover it, not just the single-batch write above.
    """
    combined = pa.table({name: table.column("c") for name, table, _ in SHAPES})

    path = tmp_path / "sliced.lance"
    with nanolance.LanceWriter(path, options=nanolance.WriteOptions(**options), max_rows_per_fragment=6_000) as writer:
        for batch in combined.to_batches(max_chunksize=1_500):
            writer.write_batch(batch)

    _assert_both_readers_agree(path, combined)


def test_a_sliced_struct_column_keeps_its_own_rows(tmp_path):
    """A sliced STRUCT is the case a sliced flat column does not cover.

    Arrow does not slice a struct's children when the struct is sliced: the parent carries the
    offset and the children keep their full extent. Reading a child array directly therefore took
    the wrong rows AND the wrong count, and the writer refused the batch outright:

        RuntimeError: column value count does not match row count for a

    Loud rather than silent, unlike the flat-column version of this bug -- but it meant
    `to_batches()`, the obvious way to feed the streaming writer, could not write a struct column at
    all. No test wrote one, because the matrix above used to exclude struct from its sliced case.
    """
    n = 2_000
    table = pa.table(
        {
            "s": pa.array([{"a": i, "b": f"v{i % 13}"} for i in range(n)]),
            "flat": pa.array(range(n), type=pa.int64()),
        }
    )

    # A single batch that is itself a slice: the narrowest form of the bug.
    one = tmp_path / "one_slice.lance"
    sliced = table.to_batches()[0].slice(500, 700)
    with nanolance.LanceWriter(one) as writer:
        writer.write_batch(sliced)
    assert pa.table(nanolance.read_table(one)).to_pydict() == pa.Table.from_batches([sliced]).to_pydict()

    # ...and the ordinary to_batches() loop across fragments.
    many = tmp_path / "many_slices.lance"
    with nanolance.LanceWriter(many, max_rows_per_fragment=800) as writer:
        for batch in table.to_batches(max_chunksize=250):
            writer.write_batch(batch)
    _assert_both_readers_agree(many, table)
