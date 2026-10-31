"""Nullable plain RLE (W1q2): a fixed-width column with nulls, no dictionary, takes `Rle{..}` + repdef as pylance 12
writes it. Runs are over the raw values of every slot, null slots included, so a null whose underlying value is 0
breaks a run, and a null that keeps its neighbour's value does not (run 1 is the probe that tells them apart).
Measured layouts are in docs/tasks/SPEC_GAPS.md ("Measured for W1q2")."""

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

_TYPES = {"int64": pa.int64(), "float64": pa.float64(), "int32": pa.int32(), "uint8": pa.uint8()}


def _values(typ: str, rows: int, run: int) -> np.ndarray:
    rng = np.random.default_rng(run * 31 + rows)
    runs = -(-rows // run)
    if typ == "int64":
        base = rng.integers(1, 2**62, runs, dtype=np.int64)
    elif typ == "float64":
        base = rng.random(runs) + 0.5
    elif typ == "int32":
        base = rng.integers(1, 2**31 - 1, runs, dtype=np.int32)
    else:
        base = rng.integers(1, 256, runs, dtype=np.uint8)
    return np.repeat(base, run)[:rows]


def _table(typ: str, rows: int, run: int, null_frac: float, under: str) -> pa.Table:
    values = _values(typ, rows, run)
    mask = np.random.default_rng(rows + 5).random(rows) < null_frac
    if under == "zero":  # null slots hold 0, which breaks the runs they sit in
        values = values.copy()
        values[mask] = 0
    # under == "hidden": a null slot keeps its run's value, so the run goes on through it
    return pa.table({"x": pa.array(values, mask=mask, type=_TYPES[typ])})


def _layout(path: Path) -> list[str]:
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    lines = [line.split("  ", 1)[1] for line in out.splitlines() if "  " in line]
    return sorted({re.sub(r",rows=\d+", "", line) for line in lines})


_RUNS = (1, 2, 5, 50)
_NULLS = (0.05, 0.5)
_UNDER = ("zero", "hidden")

# 50,000 rows for every combination; the spec's 1,000,000-row cases for int64 and float64 at runs of 2 and 5.
CASES = []
for _typ in _TYPES:
    for _run in _RUNS:
        for _frac in _NULLS:
            for _under in _UNDER:
                _id = f"{_typ}-50000-r{_run}-n{int(_frac * 100)}-{_under}"
                CASES.append(pytest.param(_typ, 50_000, _run, _frac, _under, id=_id))
for _typ in ("int64", "float64"):
    for _run in (2, 5):
        for _under in _UNDER:
            _id = f"{_typ}-1000000-r{_run}-n5-{_under}"
            CASES.append(pytest.param(_typ, 1_000_000, _run, 0.05, _under, id=_id))


@pytest.mark.parametrize("typ,rows,run,frac,under", CASES)
def test_layout_matches_pylance(tmp_path, typ, rows, run, frac, under):
    lance = require_pylance()
    table = _table(typ, rows, run, frac, under)
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")
    assert _layout(ours) == _layout(theirs), f"nanolance {_layout(ours)} pylance {_layout(theirs)}"


@pytest.mark.parametrize("typ,rows,run,frac,under", CASES)
def test_reads_both_ways_and_takes(tmp_path, typ, rows, run, frac, under):
    lance = require_pylance()
    table = _table(typ, rows, run, frac, under)
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")

    assert nl_dataset(str(ours)).to_table().equals(table)
    assert lance.dataset(str(ours)).to_table().equals(table)
    wanted = sorted(np.random.default_rng(11).choice(rows, size=200, replace=False).tolist())
    assert nl_dataset(str(ours)).take(wanted).equals(table.take(wanted))
    assert lance.dataset(str(ours)).take(wanted).equals(table.take(wanted))


@pytest.mark.parametrize("typ,rows,run,frac,under", [
    pytest.param("int64", 50_000, 2, 0.05, "zero", id="int64-r2-n5-zero"),
    pytest.param("float64", 50_000, 5, 0.5, "hidden", id="float64-r5-n50-hidden"),
    pytest.param("int32", 50_000, 1, 0.5, "zero", id="int32-r1-n50-zero"),
    pytest.param("uint8", 50_000, 50, 0.05, "zero", id="uint8-r50-n5-zero"),
])
def test_append_to_pylance_dataset(tmp_path, typ, rows, run, frac, under):
    lance = require_pylance()
    table = _table(typ, rows, run, frac, under)
    theirs = tmp_path / "theirs.lance"
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")
    extra = _table(typ, 20_000, run, frac, under)
    nl_write_dataset(extra, str(theirs), mode="append")
    expected = pa.concat_tables([table, extra])
    assert lance.dataset(str(theirs)).to_table().equals(expected)
    assert nl_dataset(str(theirs)).to_table().equals(expected)


# W1p4: a chunk that carries definition levels holds up to 4,096 rows, as pylance's do, with the levels packed over
# several 1,024-value blocks. The chunks (bytes and rows) then equal pylance's, for plain RLE and for RLE'd
# dictionary indices. 50,000 rows: one page in both writers.
@pytest.mark.parametrize(("typ", "run", "frac", "under"),
                         [("int64", 2, 0.05, "zero"), ("int64", 2, 0.5, "zero"), ("int64", 5, 0.05, "hidden"),
                          ("int64", 50, 0.05, "zero"), ("float64", 2, 0.05, "zero"), ("float64", 50, 0.5, "hidden")])
def test_chunks_match_pylance(tmp_path, typ, run, frac, under):
    from tests.test_rle_multichunk import _chunks

    lance = require_pylance()
    table = _table(typ, 50_000, run, frac, under)
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))
    assert any("Rle{" in line for line in _layout(ours)), _layout(ours)
    assert _chunks(ours, "I") == _chunks(theirs, "I")
    if run >= 5:  # long runs fill 4,096 rows before 8 KiB; runs of 2 fill 8 KiB at 1,024
        assert any(rows == 4096 for page in _chunks(ours, "I") for _, rows in page)
