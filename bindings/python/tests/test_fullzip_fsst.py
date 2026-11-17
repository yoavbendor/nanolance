"""FSST-compressed FullZip utf8 pages: Lance's per-value layout for long strings (roadmap F4).

What pylance 12.0.0 writes (measured with `build/nlance-pagelayout`): a utf8 column whose values average 128 bytes
or more is FullZip, and compressible text in it is `FullZip{values=Fsst{...,values=Variable}}`: each value compressed
on its own against one table in the page's descriptor. nanolance writes the same layout. Its table is trained once per
column (the rule the mini-block strings already follow), so the table bytes differ from pylance's; the layout strings
print only the table's size, so they compare equal. A page the table does not pay on, and a column with a value over
32 KiB, stay plain Variable. `compression=True` keeps the zstd pages, and so does a column that names its codec.
"""

from __future__ import annotations

import glob
import hashlib
import random
import re
import subprocess
from pathlib import Path

import numpy as np
import pyarrow as pa
import pytest

import nanolance
import nanolance.lance as nl
from tests.support import require_pylance

REPO = Path(__file__).resolve().parents[3]
PAGELAYOUT = REPO / "build" / "nlance-pagelayout"

WORDS = (
    "request user session timeout retry connected host port error warning latency cache miss hit queue worker "
    "batch flush"
).split()


def _text(n, width, seed, nulls=0.0):
    """`n` log-like sentences of at least `width` bytes over a small vocabulary: compressible, seeded."""
    rng = random.Random(seed)
    out = []
    for _ in range(n):
        if nulls and rng.random() < nulls:
            out.append(None)
            continue
        words = []
        size = 0
        while size < width:
            word = rng.choice(WORDS)
            words.append(word)
            size += len(word) + 1
        out.append(" ".join(words))
    return out


def _hex(n, width, seed):
    """Random hex: 16 symbols, so FSST still pays (about half the size)."""
    rng = np.random.default_rng(seed)
    return [rng.bytes(width // 2).hex() for _ in range(n)]


def _ascii(n, width, seed):
    """Random printable ASCII, 94 symbols a byte of entropy each: FSST cannot shrink it enough to pay."""
    rng = np.random.default_rng(seed)
    alphabet = np.array([chr(c) for c in range(0x21, 0x7F)])
    return ["".join(rng.choice(alphabet, width)) for _ in range(n)]


def _table(values):
    return pa.table({"s": pa.array(values, pa.utf8())})


def _layout(path: Path) -> list[str]:
    """nlance-pagelayout's report for a dataset's first page of each column, with the row count dropped."""
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    lines = [line.split("  ", 1)[1] for line in out.splitlines() if "  " in line]
    return sorted({re.sub(r",rows=\d+", "", line) for line in lines})


def _page_data_bytes(path: Path) -> list[tuple[int, int]]:
    """(rows, data-buffer bytes) of each page of the dataset's one column, from the verbose report."""
    out = subprocess.run([str(PAGELAYOUT), "-v", str(path)], capture_output=True, text=True, check=True).stdout
    pages = re.findall(r"^\s+page \d+: rows=(\d+) buffers=\[(\d+),", out, flags=re.MULTILINE)
    return [(int(rows), int(data)) for rows, data in pages]


# (name, table, expected: "fsst" = FullZip over Fsst, compared with pylance's layout; "plain" = plain Variable,
# not compared with pylance: FSST declines the page (random ASCII) or pylance uses zstd (a value over 32 KiB).)
CASES = [
    ("text_1k", _table(_text(300, 1024, seed=1)), "fsst"),
    ("text_1k_nulls", _table(_text(300, 1024, seed=2, nulls=0.10)), "fsst"),
    ("text_500", _table(_text(2_000, 500, seed=3)), "fsst"),
    # Short and long values together: the average is over 128 bytes, the page's values are over 32 KiB.
    ("mixed_short_long", _table([s if i % 2 else s[:40] for i, s in enumerate(_text(1_000, 1024, seed=4))]), "fsst"),
    ("random_hex", _table(_hex(1_000, 600, seed=5)), "fsst"),
    ("random_ascii", _table(_ascii(1_000, 600, seed=5)), "plain"),
    ("one_value_over_32k", _table(_text(200, 1024, seed=6) + ["x" * 40_000] + _text(100, 1024, seed=7)), "plain"),
]
IDS = [case[0] for case in CASES]


@pytest.mark.parametrize(("name", "table", "expected"), CASES, ids=IDS)
def test_layout_matches_pylance(tmp_path, name, table, expected):
    if expected != "fsst":
        pytest.skip("not a page pylance writes as nanolance does: see test_compressible_pages_are_fsst_and_the_rest_plain")
    lance = require_pylance()
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nl.write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")
    assert _layout(ours) == _layout(theirs)


@pytest.mark.parametrize(("name", "table", "expected"), CASES, ids=IDS)
def test_compressible_pages_are_fsst_and_the_rest_plain(tmp_path, name, table, expected):
    path = tmp_path / "ours.lance"
    nl.write_dataset(table, str(path))
    (line,) = _layout(path)
    if expected == "fsst":
        assert "FullZip{values=Fsst{symbols=2312B,values=Variable{offsets=Flat(32)}}" in line, line
    else:
        assert "FullZip{values=Variable{offsets=Flat(32)}" in line, line
        assert "Fsst" not in line, line
    assert nl.dataset(str(path)).to_table().equals(table)


@pytest.mark.parametrize(("name", "table", "expected"), CASES, ids=IDS)
def test_pylance_reads_what_nanolance_writes(tmp_path, name, table, expected):
    lance = require_pylance()
    path = tmp_path / "nl.lance"
    nl.write_dataset(table, str(path))
    assert lance.dataset(str(path)).to_table().equals(table)
    assert pa.table(nanolance.read_table(str(path))).equals(table)


@pytest.mark.parametrize(("name", "table", "expected"), CASES, ids=IDS)
def test_nanolance_reads_what_pylance_writes(tmp_path, name, table, expected):
    lance = require_pylance()
    path = tmp_path / "pylance.lance"
    lance.write_dataset(table, str(path), data_storage_version="2.2")
    assert pa.table(nanolance.read_table(str(path))).equals(table)


@pytest.mark.parametrize(("name", "table", "expected"), CASES, ids=IDS)
def test_take_and_range_match_pylance(tmp_path, name, table, expected):
    lance = require_pylance()
    path = tmp_path / "nl.lance"
    nl.write_dataset(table, str(path))
    rng = np.random.default_rng(11)
    n = table.num_rows
    idx = [int(i) for i in rng.integers(0, n, 150)] + [0, n - 1]
    got = pa.table(nanolance.take(str(path), idx))
    assert got.equals(lance.dataset(str(path)).take(idx))
    assert got.equals(table.take(pa.array(idx, pa.int64())))
    for offset, length in [(0, 7), (n // 3, n // 4), (n - 5, 5)]:
        ours = pa.table(nanolance.read_table(str(path), offset=offset, length=length))
        theirs = lance.dataset(str(path)).to_table(offset=offset, limit=length)
        assert ours.equals(theirs), (offset, length)
        assert ours.equals(table.slice(offset, length)), (offset, length)


def test_a_multi_page_column_is_fsst_on_every_page(tmp_path):
    """Pages cut at 8 MiB of values: 20k sentences of ~520 bytes are two pages. Each page's data is well under its raw
    bytes (plain FullZip is the raw bytes plus a 4-byte length a row), and both pages round-trip."""
    lance = require_pylance()
    values = _text(20_000, 520, seed=8, nulls=0.05)
    table = _table(values)
    path = tmp_path / "nl.lance"
    nl.write_dataset(table, str(path))
    pages = _page_data_bytes(path)
    assert len(pages) >= 2, pages
    start = 0
    for rows, data in pages:
        raw = sum(len(v.encode()) for v in values[start:start + rows] if v is not None)
        assert data < raw * 0.4, (start, rows, data, raw)
        start += rows
    assert start == len(values)
    assert nl.dataset(str(path)).to_table().equals(table)
    assert lance.dataset(str(path)).to_table().equals(table)
    rows = [0, 8_191, 8_192, 12_345, 19_999]
    assert pa.table(nanolance.take(str(path), rows)).equals(table.take(pa.array(rows, pa.int64())))


def test_written_files_do_not_depend_on_the_thread_count(tmp_path):
    """Four threads compress the page in parts (plan_item_fsst); the file must still match one thread's, byte for byte."""
    table = _table(_text(20_000, 400, seed=9))
    digests = []
    before = nanolance.get_threads()
    try:
        for threads in (1, 4):
            nanolance.set_threads(threads)
            path = tmp_path / f"t{threads}.lance"
            nl.write_dataset(table, str(path))
            files = sorted(glob.glob(f"{path}/data/*.lance"))
            digests.append([hashlib.sha256(Path(f).read_bytes()).hexdigest() for f in files])
            assert "Fsst{" in _layout(path)[0]
    finally:
        nanolance.set_threads(before)
    assert digests[0] and digests[0] == digests[1]


def test_plain_pages_stay_plain_when_the_writer_is_asked_for_them(tmp_path):
    """The structural switch off, a codec named as none, and zstd compression each keep the plain or zstd pages."""
    table = _table(_text(300, 1024, seed=10))
    path = tmp_path / "no_structural.lance"
    nanolance.write_table(table, path, structural_encoding=False)
    assert "Fsst" not in _layout(path)[0]

    path = tmp_path / "compressed.lance"
    nanolance.write_table(table, path, compression=True)
    assert "Fsst" not in _layout(path)[0]
    assert pa.table(nanolance.read_table(str(path))).equals(table)

    named = pa.table({"s": table.column("s")}, schema=pa.schema(
        [pa.field("s", pa.utf8(), metadata={"lance-encoding:compression": "none"})]))
    path = tmp_path / "named_none.lance"
    nl.write_dataset(named, str(path))
    assert "Fsst" not in _layout(path)[0]
    assert nl.dataset(str(path)).to_table().equals(named)
