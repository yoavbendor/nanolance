"""64-bit dictionaries whose indices are run-length encoded (W1p2): a sorted or run-heavy int64 column is written as
`Rle{Flat(32),Flat(8)}` over the u32 indices plus the dictionary, as pylance 12 writes it. The measured cases below
match pylance's layout text. Two cases at 50,000 rows do not (see the xfail marks): pylance refuses the dictionary
there and nanolance does not yet (SPEC_GAPS, "Measured for W1p2/W1p3")."""

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


def _runs(rows: int, run: int) -> np.ndarray:
    """Runs of `run` equal values, each run its own random 40-bit value."""
    rng = np.random.default_rng(run * 13 + rows)
    values = rng.integers(0, 2**40, size=-(-rows // run), dtype=np.int64)
    return np.repeat(values, run)[:rows]


def _sorted(rows: int, card: int) -> np.ndarray:
    """Sorted values drawn from `card` distinct 40-bit values."""
    rng = np.random.default_rng(card * 7 + rows)
    distinct = rng.integers(0, 2**40, size=card, dtype=np.int64)
    return np.sort(distinct[rng.integers(0, card, size=rows)])


def _layout(path: Path) -> list[str]:
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    lines = [line.split("  ", 1)[1] for line in out.splitlines() if "  " in line]
    return sorted({re.sub(r",rows=\d+", "", line) for line in lines})


XFAIL_CUTOFF = pytest.mark.xfail(
    strict=True,
    reason="pylance refuses the dictionary here; nanolance's sample rule takes it (SPEC_GAPS, W1p2 cutoff)",
)

# (name, values, expect RLE over a dictionary): the measured cases.
CASES = [
    ("runs5-50k", lambda: _runs(50_000, 5), True),
    ("runs50-50k", lambda: _runs(50_000, 50), True),
    ("runs500-50k", lambda: _runs(50_000, 500), True),
    ("runs50-1M", lambda: _runs(1_000_000, 50), True),
    ("runs500-1M", lambda: _runs(1_000_000, 500), True),
    ("runs2-1M", lambda: _runs(1_000_000, 2), False),  # no dictionary: plain RLE over the values
    ("runs5-1M", lambda: _runs(1_000_000, 5), False),
    ("sorted1000-50k", lambda: _sorted(50_000, 1_000), True),
    ("sorted2000-50k", lambda: _sorted(50_000, 2_000), True),
    ("sorted10000-50k", lambda: _sorted(50_000, 10_000), True),
    ("sorted15000-50k", lambda: _sorted(50_000, 15_000), True),
    ("sorted1000-1M", lambda: _sorted(1_000_000, 1_000), True),
    ("sorted5000-1M", lambda: _sorted(1_000_000, 5_000), True),
    ("sorted25000-1M", lambda: _sorted(1_000_000, 25_000), True),
    ("sorted100000-1M", lambda: _sorted(1_000_000, 100_000), False),  # projected at the 100,000 cap: no dictionary
]

# Measured with pylance and not matched yet: the dictionary decision at the 50,000-row cutoff.
CUTOFF_CASES = [
    ("runs2-50k", lambda: _runs(50_000, 2), False),  # sample projects to exactly rows / 2; pylance: no dictionary
    ("sorted20000-50k", lambda: _sorted(50_000, 20_000), False),
    ("sorted25000-50k", lambda: _sorted(50_000, 25_000), False),
]


def _has_rle_dict(layout: list[str]) -> bool:
    return any("Rle{values=Flat(32),lengths=Flat(8)}" in line and "dict=" in line for line in layout)


@pytest.mark.parametrize("name,make,expect", CASES, ids=[c[0] for c in CASES])
def test_rle_over_dictionary_layout_matches_pylance(tmp_path, name, make, expect):
    lance = require_pylance()
    table = pa.table({"x": pa.array(make())})
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))

    ours_layout = _layout(ours)
    assert ours_layout == _layout(theirs), f"nanolance {ours_layout} pylance {_layout(theirs)}"
    assert _has_rle_dict(ours_layout) == expect


@pytest.mark.parametrize("name,make,expect", CASES[:6], ids=[c[0] for c in CASES[:6]])
def test_rle_over_dictionary_reads_and_takes(tmp_path, name, make, expect):
    lance = require_pylance()
    table = pa.table({"x": pa.array(make())})
    ours = tmp_path / "ours.lance"
    nl_write_dataset(table, str(ours))

    expected = table.column("x").to_pylist()
    assert nl_dataset(str(ours)).to_table().column("x").to_pylist() == expected
    assert lance.dataset(str(ours)).to_table().column("x").to_pylist() == expected

    rng = np.random.default_rng(11)
    wanted = sorted(rng.choice(len(expected), size=200, replace=False).tolist())
    assert lance.dataset(str(ours)).take(wanted).column("x").to_pylist() == [expected[i] for i in wanted]
    assert nl_dataset(str(ours)).take(wanted).column("x").to_pylist() == [expected[i] for i in wanted]


@pytest.mark.parametrize("name,make,expect", [CASES[0], CASES[3], CASES[7], CASES[12]],
                         ids=["runs5-50k", "runs50-1M", "sorted1000-50k", "sorted25000-1M"])
def test_append_to_pylance_written_rle_dictionary(tmp_path, name, make, expect):
    lance = require_pylance()
    values = make()
    table = pa.table({"x": pa.array(values)})
    theirs = tmp_path / "theirs.lance"
    lance.write_dataset(table, str(theirs))

    extra = pa.table({"x": pa.array(values[: len(values) // 4])})
    nl_write_dataset(extra, str(theirs), mode="append")

    expected = table.column("x").to_pylist() + extra.column("x").to_pylist()
    assert lance.dataset(str(theirs)).to_table().column("x").to_pylist() == expected
    assert nl_dataset(str(theirs)).to_table().column("x").to_pylist() == expected


@pytest.mark.parametrize("name,make,expect", [pytest.param(c[0], c[1], c[2], marks=XFAIL_CUTOFF, id=c[0])
                                              for c in CUTOFF_CASES])
def test_cutoff_layout_matches_pylance(tmp_path, name, make, expect):
    lance = require_pylance()
    table = pa.table({"x": pa.array(make())})
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))
    assert _layout(ours) == _layout(theirs)
