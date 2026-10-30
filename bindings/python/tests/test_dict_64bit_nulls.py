"""Nullable 64-bit dictionary columns (W1p3), as pylance 12 writes them: `InlineBitpacking(32)` indices (or
`Rle{Flat(32),Flat(8)}` for a sorted or run-heavy column) with `repdef=Bitpacked(16,Flat(1))`. Lance hashes every
slot's raw value, a null slot's too, so the value under the nulls is a dictionary entry when no valid row holds it
(`x11` for 10 distinct values with zeros under the nulls, `x10` when the nulls hide an existing value).

Known difference (W1p4): pylance's RLE chunks with definition levels hold 4,096 rows, nanolance's 1,024 (its
definition-level packer covers one FastLanes block). The layout text is the same and both read either file."""

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

PAGELAYOUT = Path(__file__).resolve().parents[3] / "build" / "nlance-pagelayout"
ROWS = 50_000


def _layout(path: Path) -> list[str]:
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    lines = [line.split("  ", 1)[1] for line in out.splitlines() if "  " in line]
    return sorted({re.sub(r",rows=\d+", "", line) for line in lines})


def _column(values: np.ndarray, null_fraction: float, under: int | None, seed: int = 1) -> pa.Array:
    rng = np.random.default_rng(seed)
    mask = rng.random(len(values)) < null_fraction
    values = values.copy()
    if under is not None:
        values[mask] = under
    return pa.Array.from_pandas(values, mask=mask, type=pa.int64())


def _cardinality(k: int, rows: int = ROWS) -> np.ndarray:
    rng = np.random.default_rng(k)
    return rng.integers(1, 1 << 40, k)[rng.integers(0, k, rows)]


CASES = {
    "k10-5pct-under0": lambda: _column(_cardinality(10), 0.05, 0),
    "k10-50pct-under0": lambda: _column(_cardinality(10), 0.5, 0),
    "k10-5pct-under-existing": lambda: _column(_cardinality(10), 0.05, None),
    "k1000-5pct-under0": lambda: _column(_cardinality(1000), 0.05, 0),
    "k1000-50pct-under-existing": lambda: _column(_cardinality(1000), 0.5, None),
    "sorted1000-5pct": lambda: _column(np.sort(np.random.default_rng(3).integers(1, 1000, ROWS)) * 1_000_003, 0.05, 0),
    "runs50-5pct": lambda: _column(np.repeat(np.random.default_rng(4).integers(1, 1 << 40, ROWS // 50), 50), 0.05, 0),
    "k10-5pct-1m": lambda: _column(_cardinality(10, 1_000_000), 0.05, 0),
}


@pytest.mark.parametrize("name", sorted(CASES))
def test_nullable_dictionary_matches_pylance(tmp_path, name):
    lance = require_pylance()
    table = pa.table({"x": CASES[name]()})
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))
    layout = _layout(ours)
    assert any("dict=" in line and "nullable" in line for line in layout), layout
    assert layout == _layout(theirs)
    for read in (nl_dataset(str(ours)), lance.dataset(str(ours)), nl_dataset(str(theirs))):
        assert read.to_table() == table
    picks = [0, 1, 7, table.num_rows // 2, table.num_rows - 1]
    assert nl_dataset(str(ours)).take(picks) == table.take(picks)
    assert lance.dataset(str(ours)).take(picks) == table.take(picks)


def test_value_under_the_nulls_is_an_entry_only_when_new(tmp_path):
    lance = require_pylance()
    for under, extra in ((0, 1), (None, 0)):
        table = pa.table({"x": _column(_cardinality(10), 0.05, under)})
        path = tmp_path / f"u{extra}.lance"
        nl_write_dataset(table, str(path))
        assert any(f"x{10 + extra}," in line for line in _layout(path)), _layout(path)


def test_append_to_pylance_dataset(tmp_path):
    lance = require_pylance()
    first = pa.table({"x": _column(_cardinality(10), 0.05, 0, seed=5)})
    second = pa.table({"x": _column(_cardinality(10), 0.05, 0, seed=6)})
    path = tmp_path / "d.lance"
    lance.write_dataset(first, str(path))
    nl_write_dataset(second, str(path), mode="append")
    expected = pa.concat_tables([first, second])
    assert lance.dataset(str(path)).to_table() == expected
    assert nl_dataset(str(path)).to_table() == expected
