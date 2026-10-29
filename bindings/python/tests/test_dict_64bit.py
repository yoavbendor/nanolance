"""64-bit fixed-width dictionaries (W1p): when nanolance dictionary-encodes an int64, uint64, timestamp or date64
column, and how the dictionary is stored, as pylance 12 does (Lance's should_dictionary_encode and its
dictionary-values choice). Nullable and sorted columns are not covered here (W1p3, W1p2)."""

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


def _values(card: int, rows: int, bits: int = 40) -> np.ndarray:
    """Shuffled values drawn from `card` distinct `bits`-wide values (the measured shape: random, not runs)."""
    rng = np.random.default_rng(card * 7 + rows)
    distinct = rng.integers(0, 2**bits, size=card, dtype=np.int64)
    return distinct[rng.integers(0, card, size=rows)]


def _array(typ: str, values: np.ndarray) -> pa.Array:
    if typ == "int64":
        return pa.array(values)
    if typ == "uint64":
        return pa.array(values.astype(np.uint64))
    if typ == "timestamp":
        return pa.array(values, type=pa.timestamp("us"))
    if typ == "date64":
        return pa.array(values, type=pa.date64())
    raise ValueError(typ)


def _layout(path: Path) -> list[str]:
    """nlance-pagelayout's encoding per page, the row count dropped (as test_column_codec compares them)."""
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    lines = [line.split("  ", 1)[1] for line in out.splitlines() if "  " in line]
    return sorted({re.sub(r",rows=\d+", "", line) for line in lines})


def _has_dict(layout: list[str]) -> bool:
    return any("dict=" in line for line in layout)


# (type, rows, distinct values): the dictionary cases pylance was measured on, plus the ones it refuses.
CASES = [
    ("int64", 50_000, 10),
    ("int64", 50_000, 100),
    ("int64", 50_000, 200),
    ("int64", 50_000, 500),
    ("int64", 50_000, 1_000),
    ("int64", 50_000, 2_000),
    ("uint64", 50_000, 100),
    ("uint64", 50_000, 1_000),
    ("timestamp", 50_000, 100),
    ("timestamp", 50_000, 1_000),
    ("date64", 50_000, 10),
    ("date64", 50_000, 1_000),
    ("int64", 50_000, 5_000),  # sample projects past rows / 2: no dictionary
    ("int64", 1_000_000, 10),
    ("int64", 1_000_000, 1_000),  # projected past 100,000: no dictionary
]


@pytest.mark.parametrize("typ,rows,card", CASES, ids=[f"{t}-{r}-{c}" for t, r, c in CASES])
def test_dictionary_layout_matches_pylance(tmp_path, typ, rows, card):
    lance = require_pylance()
    table = pa.table({"x": _array(typ, _values(card, rows))})

    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))

    ours_layout = _layout(ours)
    theirs_layout = _layout(theirs)
    assert ours_layout == theirs_layout, f"nanolance {ours_layout} pylance {theirs_layout}"
    # The dictionary decision is pylance's: whether it is there is checked by the layout equality above.
    assert _has_dict(theirs_layout) == _has_dict(ours_layout)


@pytest.mark.parametrize("typ,rows,card", [CASES[1], CASES[5], CASES[9], CASES[13]])
def test_dictionary_values_read_back(tmp_path, typ, rows, card):
    lance = require_pylance()
    table = pa.table({"x": _array(typ, _values(card, rows))})
    ours = tmp_path / "ours.lance"
    nl_write_dataset(table, str(ours))

    expected = table.column("x").to_pylist()
    assert nl_dataset(str(ours)).to_table().column("x").to_pylist() == expected
    assert lance.dataset(str(ours)).to_table().column("x").to_pylist() == expected

    rng = np.random.default_rng(3)
    rows_wanted = sorted(rng.choice(rows, size=200, replace=False).tolist())
    taken = lance.dataset(str(ours)).take(rows_wanted).column("x").to_pylist()
    assert taken == [expected[i] for i in rows_wanted]
