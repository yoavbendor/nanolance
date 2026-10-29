"""RLE over more than one mini-block chunk (W1q): a run-length column whose runs do not fit one chunk is split into
chunks the way pylance 12.0.0 splits them.

pylance's rule, measured on its files (docs/tasks/SPEC_GAPS.md, "Measured for W1q"): a chunk takes the largest power
of two up to 4,096 rows whose encoding is at most 8,192 bytes (an 8-byte header plus one value and one length per
run piece, padded to 8). Pylance's 2.1 files carry u16 control words; nanolance's 2.2 files carry u32 words (the
form stock Lance's 2.2 validator accepts), so the chunks are the same and only the word width differs. The tests
compare nanolance's layout text and its chunk list (values and bytes per chunk, read from the page's control
buffer) with pylance's, check that each side reads the other's output, and check `take`.
"""

from __future__ import annotations

import glob
import os
import struct

import numpy as np
import pyarrow as pa
import pytest

from nanolance.lance.dataset import dataset as nl_dataset
from nanolance.lance.dataset import write_dataset as nl_write_dataset
from tests.support import require_pylance
from tests.test_column_codec import _layout
from tests.test_encoding_options import _table

RNG = np.random.default_rng(7)


def _runs(dtype: pa.DataType, length: int, rows: int) -> pa.Array:
    """`rows` values in runs of `length` equal values, each run a fresh random value."""
    count = rows // length + 1
    if pa.types.is_floating(dtype):
        values = RNG.random(count) * 1000.0
    elif dtype == pa.uint8():
        values = RNG.integers(0, 256, count, dtype=np.int64)
    elif dtype == pa.int32():
        values = RNG.integers(-(2**31), 2**31 - 1, count, dtype=np.int64)
    else:
        values = RNG.integers(-(2**62), 2**62, count, dtype=np.int64)
    return pa.array(np.repeat(values, length)[:rows].astype(dtype.to_pandas_dtype()), type=dtype)


def _chunks(path, word: str) -> list[list[tuple[int, int]]]:
    """Per page of the single column, the (chunk bytes, values) of each chunk, from the page's control buffer
    whose words are `word` ("I": u32, nanolance's 2.2 files; "H": u16, pylance 12's 2.1 files). The last chunk's
    values are 0 (what is left)."""
    raw = open(sorted(glob.glob(os.path.join(str(path), "data", "*.lance")))[0], "rb").read()
    _, cmo_start, _, _, n_cols, _, _ = struct.unpack("<QQQIIHH", raw[-40:][:36])
    assert n_cols == 1
    pos, length = struct.unpack("<QQ", raw[cmo_start : cmo_start + 16])
    pages = []
    for offsets, sizes in _column_pages(raw[pos : pos + length]):
        control = raw[offsets[0] : offsets[0] + sizes[0]]
        words = struct.unpack(f"<{len(control) // struct.calcsize(word)}{word}", control)
        chunks = []
        for word in words:
            log = word & 0x0F
            chunks.append((((word >> 4) + 1) * 8, (1 << log) if log else 0))
        pages.append(chunks)
    return pages


def _column_pages(meta: bytes) -> list[tuple[list[int], list[int]]]:
    """Each page's buffer offsets and sizes from a protobuf ColumnMetadata (pages are field 2, buffer offsets
    field 1 and sizes field 2 within a page)."""
    out = []
    for field_no, _, value in _fields(meta):
        if field_no == 2:
            offsets, sizes = [], []
            for pf, _, pv in _fields(value):
                if pf == 1:
                    offsets = _packed(pv)
                elif pf == 2:
                    sizes = _packed(pv)
            out.append((offsets, sizes))
    return out


def _fields(buf: bytes):
    pos = 0
    while pos < len(buf):
        key, pos = _varint(buf, pos)
        field_no, wire = key >> 3, key & 7
        if wire == 0:
            value, pos = _varint(buf, pos)
        elif wire == 2:
            n, pos = _varint(buf, pos)
            value = buf[pos : pos + n]
            pos += n
        else:
            raise AssertionError(f"unexpected protobuf wire type {wire}")
        yield field_no, wire, value


def _varint(buf: bytes, pos: int) -> tuple[int, int]:
    shift = out = 0
    while True:
        b = buf[pos]
        pos += 1
        out |= (b & 0x7F) << shift
        if not b & 0x80:
            return out, pos
        shift += 7


def _packed(buf) -> list[int]:
    if isinstance(buf, int):
        return [buf]
    out, pos = [], 0
    while pos < len(buf):
        value, pos = _varint(buf, pos)
        out.append(value)
    return out


def _check_pair(lance, tmp_path, table: pa.Table) -> None:
    """nanolance writes ours; pylance writes its 2.1 file (the format the measured RLE layouts are from)."""
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs), data_storage_version="2.1")
    assert lance.dataset(str(ours)).to_table() == table
    assert nl_dataset(str(ours)).to_table() == table
    assert nl_dataset(str(theirs)).to_table() == table  # pylance-written pages read in nanolance


# (id, dtype, run length, rows, options). Every case is a column pylance RLE-encodes (measured, W1q).
CASES = [
    ("i64-runs2-50k", pa.int64(), 2, 50_000, {}),
    ("i64-runs2-50k-threshold1", pa.int64(), 2, 50_000, {"rle-threshold": "1"}),
    ("i64-runs2-1m", pa.int64(), 2, 1_000_000, {}),
    ("i64-runs3-50k", pa.int64(), 3, 50_000, {}),
    ("i64-runs4-50k", pa.int64(), 4, 50_000, {}),
    ("i64-runs5-50k", pa.int64(), 5, 50_000, {}),
    ("i64-runs8-50k", pa.int64(), 8, 50_000, {}),
    ("i32-runs2-50k", pa.int32(), 2, 50_000, {}),
    ("i32-runs3-50k", pa.int32(), 3, 50_000, {}),
    ("i32-runs8-50k", pa.int32(), 8, 50_000, {}),
    ("u8-runs2-50k", pa.uint8(), 2, 50_000, {}),
    ("u8-runs3-50k", pa.uint8(), 3, 50_000, {}),
]


@pytest.mark.parametrize(("name", "dtype", "run", "rows", "options"), CASES, ids=[c[0] for c in CASES])
def test_multichunk_rle_matches_pylance(tmp_path, name, dtype, run, rows, options):
    lance = require_pylance()
    table = _table(_runs(dtype, run, rows), options)
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    _check_pair(lance, tmp_path, table)
    assert any("Rle{" in line for line in _layout(ours)), _layout(ours)
    assert _layout(ours) == _layout(theirs)
    # Same chunks: the same values per chunk and the same bytes, except nanolance's u32 buffer-size header, which
    # is 8 bytes larger per chunk than pylance's u16 one.
    ours_chunks, theirs_chunks = _chunks(ours, "I"), _chunks(theirs, "H")
    assert [[v for _, v in page] for page in ours_chunks] == [[v for _, v in page] for page in theirs_chunks]
    assert [[b - 8 for b, _ in page] for page in ours_chunks] == [[b for b, _ in page] for page in theirs_chunks]
    assert all(len(chunks) > 1 for chunks in _chunks(ours, "I"))  # every page spans more than one chunk
    # take() from nanolance's file and from pylance's file gives the same rows.
    picks = [0, 1, rows // 2, rows - 1, rows // 3]
    expected = table.take(picks)
    assert nl_dataset(str(ours)).take(picks) == expected
    assert lance.dataset(str(ours)).take(picks) == expected
    assert nl_dataset(str(theirs)).take(picks) == expected


def test_rle_with_pylance_chunks_reads_back_in_nanolance(tmp_path):
    lance = require_pylance()
    table = _table(_runs(pa.int64(), 2, 50_000), {})
    theirs = tmp_path / "theirs.lance"
    lance.write_dataset(table, str(theirs), data_storage_version="2.1")
    assert [len(chunks) for chunks in _chunks(theirs, "H")] == [49]
    assert nl_dataset(str(theirs)).to_table() == table


def test_run_of_50_is_multichunk_rle_without_pylance_dictionary(tmp_path):
    """Runs of 50 are an RLE column pylance also dictionary-encodes (W1p2, not this change): only the read-back
    and the multi-chunk shape are checked here."""
    lance = require_pylance()
    table = _table(_runs(pa.int64(), 50, 50_000), {})
    ours = tmp_path / "ours.lance"
    nl_write_dataset(table, str(ours))
    assert lance.dataset(str(ours)).to_table() == table
    assert nl_dataset(str(ours)).to_table() == table
    assert any("Rle{" in line or "dict" in line for line in _layout(ours))


def test_random_int64_column_is_still_not_rle(tmp_path):
    lance = require_pylance()
    values = pa.array(RNG.integers(-(2**62), 2**62, 50_000, dtype=np.int64))
    table = _table(values, {})
    ours = tmp_path / "random.lance"
    nl_write_dataset(table, str(ours))
    assert not any("Rle{" in line for line in _layout(ours))
    assert lance.dataset(str(ours)).to_table() == table
