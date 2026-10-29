"""Fixed-width FullZip pages: Lance 12's 256-byte rule, lance-encoding:structural-encoding, and their reads.

What pylance 12.0.0 writes (measured with `build/nlance-pagelayout`, docs/tasks/SPEC_GAPS.md "Measured for
W1i"): a fixed-width value of 256 bytes or more is FullZip, a narrower one mini-block; the structural option
overrides that. A wide fixed_size_list is `FullZip{values=FixedSizeList{NxFlat(b)}}`, a fixed_size_binary
`FullZip{values=Flat(b)}`, with `def_bits=1` when rows are null. Pylance's page split is not reproduced: pylance
cuts a fixed-width FullZip column at its input's batch boundaries, nanolance at 8 MiB of row data.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import numpy as np
import pyarrow as pa
import pytest

from nanolance.lance.dataset import dataset as nl_dataset
from nanolance.lance.dataset import write_dataset as nl_write_dataset
from tests.support import require_pylance

ROWS = 2_000
REPO = Path(__file__).resolve().parents[3]
PAGELAYOUT = REPO / "build" / "nlance-pagelayout"
STRUCTURAL = "lance-encoding:structural-encoding"


def _fsl(dim: int, rows: int = ROWS, *, row_nulls: bool = False, item_nulls: bool = False, int8: bool = False):
    rng = np.random.default_rng(dim + rows)
    if int8:
        values = rng.integers(-128, 127, rows * dim, dtype=np.int8)
        item_type = pa.int8()
    else:
        values = rng.random(rows * dim).astype(np.float32)
        item_type = pa.float32()
    item_mask = rng.random(rows * dim) < 0.1 if item_nulls else None
    items = pa.array(values, type=item_type, mask=item_mask)
    row_mask = pa.array(rng.random(rows) < 0.2) if row_nulls else None
    return pa.FixedSizeListArray.from_arrays(items, dim, mask=row_mask)


def _fsb(width: int, rows: int = ROWS, *, nulls: bool = False):
    rng = np.random.default_rng(width + rows)
    data = [bytes(rng.integers(0, 256, width, dtype=np.uint8)) for _ in range(rows)]
    if nulls:
        data = [None if rng.random() < 0.2 else d for d in data]
    return pa.array(data, type=pa.binary(width))


def _ints(rows: int = ROWS):
    return pa.array(np.random.default_rng(7).integers(0, 1 << 40, rows, dtype=np.int64))


def _table(arr: pa.Array, option: str | None = None) -> pa.Table:
    metadata = {STRUCTURAL.encode(): option.encode()} if option else None
    field = pa.field("c", arr.type, metadata=metadata)
    return pa.table({"c": arr}, schema=pa.schema([field]))


def _layout(path: Path) -> list[str]:
    """nlance-pagelayout's report for a dataset's first page of each column, with the row count dropped."""
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    lines = [line.split("  ", 1)[1] for line in out.splitlines() if "  " in line]
    return sorted({re.sub(r",rows=\d+", "", line) for line in lines})


# (name, array, structural option). Around the 256-byte threshold, the vector widths, nulls, and the overrides.
CASES = [
    ("fsl63", _fsl(63), None),  # 252 bytes: mini-block
    ("fsl64", _fsl(64), None),  # 256 bytes: FullZip
    ("fsl64_row_nulls", _fsl(64, row_nulls=True), None),
    ("fsl64_item_nulls", _fsl(64, item_nulls=True), None),
    ("fsl63_item_nulls", _fsl(63, item_nulls=True), None),  # validity is not in the width: mini-block
    ("fsl768", _fsl(768), None),
    ("fsl768_row_nulls", _fsl(768, row_nulls=True), None),
    ("fsl768_item_nulls", _fsl(768, item_nulls=True), None),
    ("fsl1536", _fsl(1536), None),
    ("fsl4096", _fsl(4096), None),
    ("int8_fsl768", _fsl(768, int8=True), None),
    ("fsb255", _fsb(255), None),
    ("fsb256", _fsb(256), None),
    ("fsb256_nulls", _fsb(256, nulls=True), None),
    ("fsb4096", _fsb(4096), None),
    ("fsl16_fullzip", _fsl(16), "fullzip"),
    ("fsl16_nulls_fullzip", _fsl(16, row_nulls=True), "fullzip"),
    ("fsl768_miniblock", _fsl(768), "miniblock"),
    ("int64_fullzip", _ints(), "fullzip"),
    ("int64_miniblock", _ints(), "miniblock"),
    ("fsb100_fullzip", _fsb(100), "fullzip"),
]
IDS = [case[0] for case in CASES]


@pytest.mark.parametrize(("name", "arr", "option"), CASES, ids=IDS)
def test_layout_matches_pylance(tmp_path, name, arr, option):
    lance = require_pylance()
    table = _table(arr, option)
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")
    assert _layout(ours) == _layout(theirs)


@pytest.mark.parametrize(("name", "arr", "option"), CASES, ids=IDS)
def test_pylance_reads_what_nanolance_writes(tmp_path, name, arr, option):
    lance = require_pylance()
    table = _table(arr, option)
    path = tmp_path / "nl.lance"
    nl_write_dataset(table, str(path))
    assert lance.dataset(str(path)).to_table() == table
    assert nl_dataset(str(path)).to_table() == table


@pytest.mark.parametrize(("name", "arr", "option"), CASES, ids=IDS)
def test_nanolance_reads_what_pylance_writes(tmp_path, name, arr, option):
    lance = require_pylance()
    table = _table(arr, option)
    path = tmp_path / "pylance.lance"
    lance.write_dataset(table, str(path), data_storage_version="2.2")
    assert nl_dataset(str(path)).to_table() == table


@pytest.mark.parametrize(("name", "arr", "option"), CASES, ids=IDS)
def test_take_matches_pylance(tmp_path, name, arr, option):
    lance = require_pylance()
    import nanolance

    table = _table(arr, option)
    path = tmp_path / "nl.lance"
    nl_write_dataset(table, str(path))
    rng = np.random.default_rng(11)
    idx = [int(i) for i in rng.integers(0, ROWS, 200)] + [0, ROWS - 1]
    got = pa.table(nanolance.take(str(path), idx))
    assert got.to_pydict() == lance.dataset(str(path)).take(idx).to_pydict()
    assert got.to_pydict() == table.take(pa.array(idx, pa.int64())).to_pydict()


def test_wide_fsl_is_a_fullzip_page_and_round_trips(tmp_path):
    # Without pylance: a 256-byte fixed_size_list column is written FullZip and read back equal.
    table = _table(_fsl(64, row_nulls=True))
    path = tmp_path / "nl.lance"
    nl_write_dataset(table, str(path))
    assert nl_dataset(str(path)).to_table() == table
    if PAGELAYOUT.exists():
        assert all("FullZip{" in line for line in _layout(path))


@pytest.mark.parametrize("option", ["fullzip", "miniblock"])
def test_structural_encoding_round_trips_on_narrow_and_wide(tmp_path, option):
    table = _table(_fsl(16), option)
    path = tmp_path / "nl.lance"
    nl_write_dataset(table, str(path))
    assert nl_dataset(str(path)).to_table() == table


def test_unknown_structural_encoding_is_refused(tmp_path):
    table = _table(_ints(), "bogus")
    with pytest.raises(Exception, match="structural encoding"):
        nl_write_dataset(table, str(tmp_path / "nl.lance"))


def test_fullzip_on_a_bool_column_is_refused_until_nanolance_writes_it(tmp_path):
    # pylance 12 writes a bool column as FullZip{Flat(8)} when asked; nanolance keeps bools bit-packed and says so
    # by name instead of writing another layout (docs/tasks/SPEC_GAPS.md, "Measured for W1i": left).
    arr = pa.array(np.arange(ROWS) % 2 == 0)
    with pytest.raises(Exception, match="not written yet"):
        nl_write_dataset(_table(arr, "fullzip"), str(tmp_path / "nl.lance"))
