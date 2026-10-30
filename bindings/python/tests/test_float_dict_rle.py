"""Float columns take the integer layouts on their raw bits (W1s): a float64 dictionary, RLE over values or indices, and
InlineBitpacking for values whose sign bit is clear, as pylance 12 writes them. Measured layouts are in
docs/tasks/SPEC_GAPS.md ("Measured for W1s"). Floats are compared bit for bit: NaN payloads and -0.0 survive."""

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

REPO = Path(__file__).resolve().parents[3]
PAGELAYOUT = REPO / "build" / "nlance-pagelayout"


def _random(rows: int) -> np.ndarray:
    return np.random.default_rng(rows).random(rows)


def _distinct(rows: int, card: int) -> np.ndarray:
    rng = np.random.default_rng(card * 7 + rows)
    return rng.random(card)[rng.integers(0, card, rows)]


def _runs(rows: int, run: int) -> np.ndarray:
    rng = np.random.default_rng(run * 13 + rows)
    return np.repeat(rng.random(-(-rows // run)), run)[:rows]


def _sorted(rows: int, card: int) -> np.ndarray:
    rng = np.random.default_rng(card * 17 + rows)
    return np.sort(rng.random(card)[rng.integers(0, card, rows)])


def _layout(path: Path) -> list[str]:
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    lines = [line.split("  ", 1)[1] for line in out.splitlines() if "  " in line]
    return sorted({re.sub(r",rows=\d+", "", line) for line in lines})


# (name, values, nulls): float64 cases, layout checked against pylance 12 (2.2 files).
_F64 = [
    ("random", _random),
    ("distinct10", lambda n: _distinct(n, 10)),
    ("distinct1000", lambda n: _distinct(n, 1_000)),
    ("distinct5000", lambda n: _distinct(n, 5_000)),
    ("runs2", lambda n: _runs(n, 2)),
    ("runs5", lambda n: _runs(n, 5)),
    ("runs50", lambda n: _runs(n, 50)),
    ("sorted1000", lambda n: _sorted(n, 1_000)),
    ("sorted5000", lambda n: _sorted(n, 5_000)),
]
PLAIN = []  # (name, make, rows, nulls) for the read and take checks: every case reads back
CASES = []  # the same for the layout check (nullable plain RLE matches pylance since W1q2)
for name, make in _F64:
    for rows in (50_000, 1_000_000):
        for nulls in (False, True):
            case_id = f"{name}-{rows}-{'nulls' if nulls else 'dense'}"
            PLAIN.append(pytest.param(name, make, rows, nulls, id=case_id))
            CASES.append(pytest.param(name, make, rows, nulls, id=case_id))


def _table(make, rows: int, nulls: bool) -> pa.Table:
    values = make(rows)
    mask = np.random.default_rng(rows + 5).random(rows) < 0.05 if nulls else None
    return pa.table({"x": pa.array(values, mask=mask)})


def _bits(array: pa.Array) -> np.ndarray:
    """The raw 64-bit patterns, nulls as zero: NaN payloads and signed zeros compare exactly."""
    return array.fill_null(0.0).to_numpy(zero_copy_only=False).view(np.int64)


def _assert_bit_equal(got: pa.Table, want: pa.Table) -> None:
    assert got.num_rows == want.num_rows
    assert got.column("x").is_null().equals(want.column("x").is_null())
    assert np.array_equal(_bits(got.column("x")), _bits(want.column("x")))


@pytest.mark.parametrize("name,make,rows,nulls", CASES)
def test_float64_layout_matches_pylance(tmp_path, name, make, rows, nulls):
    lance = require_pylance()
    table = _table(make, rows, nulls)
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")
    assert _layout(ours) == _layout(theirs), f"nanolance {_layout(ours)} pylance {_layout(theirs)}"


@pytest.mark.parametrize("name,make,rows,nulls", PLAIN)
def test_float64_reads_bit_exact_and_takes(tmp_path, name, make, rows, nulls):
    lance = require_pylance()
    table = _table(make, rows, nulls)
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")

    _assert_bit_equal(nl_dataset(str(ours)).to_table(), table)
    _assert_bit_equal(lance.dataset(str(ours)).to_table(), table)
    wanted = sorted(np.random.default_rng(11).choice(rows, size=200, replace=False).tolist())
    _assert_bit_equal(nl_dataset(str(ours)).take(wanted), table.take(wanted))
    _assert_bit_equal(lance.dataset(str(ours)).take(wanted), table.take(wanted))


@pytest.mark.parametrize("name,make,rows,nulls", [CASES[1], CASES[6], CASES[13], CASES[0]],
                         ids=["distinct10", "runs50", "sorted1000", "random"])
def test_float64_append_to_pylance_dataset(tmp_path, name, make, rows, nulls):
    lance = require_pylance()
    table = _table(make, 50_000, nulls)
    theirs = tmp_path / "theirs.lance"
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")
    extra = _table(make, 20_000, nulls)
    nl_write_dataset(extra, str(theirs), mode="append")
    expected = pa.concat_tables([table, extra])
    _assert_bit_equal(lance.dataset(str(theirs)).to_table(), expected)
    _assert_bit_equal(nl_dataset(str(theirs)).to_table(), expected)


def test_float64_dictionary_keeps_nan_payloads_and_signed_zeros(tmp_path):
    """Raw-bit hashing: two NaN payloads, -0.0 and +0.0 are four dictionary entries, not two."""
    lance = require_pylance()
    rng = np.random.default_rng(3)
    nan_a = np.array([0x7FF8000000000001], dtype=np.uint64).view(np.float64)[0]
    nan_b = np.array([0x7FF8000000000002], dtype=np.uint64).view(np.float64)[0]
    palette = np.array([nan_a, nan_b, -0.0, 0.0, 1.5])
    values = palette[rng.integers(0, len(palette), 50_000)]
    table = pa.table({"x": pa.array(values)})
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")
    assert _layout(ours) == _layout(theirs)
    assert any("dict=" in line for line in _layout(ours)), "expected a dictionary for five distinct bit patterns"
    _assert_bit_equal(nl_dataset(str(ours)).to_table(), table)
    _assert_bit_equal(lance.dataset(str(ours)).to_table(), table)


def test_float64_with_own_codec_keeps_flat_layout(tmp_path):
    """A float column with its own lance-encoding:compression keeps its layout (no dictionary, RLE or bitpacking)."""
    values = _runs(50_000, 50)
    field = pa.field("x", pa.float64(), metadata={"lance-encoding:compression": "none"})
    table = pa.table({"x": pa.array(values)}, schema=pa.schema([field]))
    ours = tmp_path / "ours.lance"
    nl_write_dataset(table, str(ours))
    layout = _layout(ours)
    assert not any("dict=" in line or "Rle" in line or "InlineBitpacking" in line for line in layout), layout
    _assert_bit_equal(nl_dataset(str(ours)).to_table(), table)


@pytest.mark.parametrize("rows", [50_000, 1_000_000], ids=["50k", "1M"])
@pytest.mark.parametrize("run", [2, 50], ids=["runs2", "runs50"])
def test_float32_runs_use_rle_as_pylance_writes_them(tmp_path, rows, run):
    lance = require_pylance()
    values = _runs(rows, run).astype(np.float32)
    table = pa.table({"x": pa.array(values)})
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")
    assert _layout(ours) == _layout(theirs)
    got = nl_dataset(str(ours)).to_table().column("x").to_numpy().view(np.int32)
    assert np.array_equal(got, values.view(np.int32))
