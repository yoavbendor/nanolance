"""`lance-encoding:compression` (lz4 | zstd | none) and `lance-encoding:compression-level` on one field.

The layouts asserted here are what pylance 12.0.0 writes for the same field metadata (measured with
`build/nlance-pagelayout`, one 50,000-row column per case). Where nanolance's layout differs from pylance's
the case is listed in KNOWN_LAYOUT_GAPS with the reason, so a change that closes a gap shows up as a test
that now passes unexpectedly.
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

ROWS = 50_000
REPO = Path(__file__).resolve().parents[3]
PAGELAYOUT = REPO / "build" / "nlance-pagelayout"


def _column(kind: str) -> pa.Array:
    rng = np.random.default_rng(1)
    if kind == "int64":
        return pa.array(rng.integers(0, 1 << 40, ROWS, dtype=np.int64))
    if kind == "int32":
        return pa.array(rng.integers(0, 1 << 30, ROWS, dtype=np.int32))
    if kind == "float64":
        return pa.array(rng.random(ROWS))
    if kind == "float32":
        return pa.array(rng.random(ROWS).astype(np.float32))
    if kind == "string":
        return pa.array([f"unique-{i:08d}-" + "x" * (i % 7) for i in range(ROWS)])
    if kind == "bool":
        return pa.array(rng.random(ROWS) < 0.5)
    if kind == "fsl":
        return pa.FixedSizeListArray.from_arrays(pa.array(rng.random(ROWS * 4).astype(np.float32)), 4)
    raise ValueError(kind)


def _table(kind: str, metadata: dict[bytes, bytes]) -> pa.Table:
    arr = _column(kind)
    return pa.table({"c": arr}, schema=pa.schema([pa.field("c", arr.type, metadata=metadata)]))


def _opt(name: str, value: str) -> dict[bytes, bytes]:
    return {f"lance-encoding:{name}".encode(): value.encode()}


def _layout(path: Path) -> list[str]:
    """The encodings of nlance-pagelayout's report for a dataset, one per page with the row count dropped:
    `<name>  <column> (<type>): <layout>` becomes `<column> (<type>): <layout>`. Page sizes are the writer's
    choice; the encoding is what this compares."""
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    lines = [line.split("  ", 1)[1] for line in out.splitlines() if "  " in line]
    return sorted({re.sub(r",rows=\d+", "", line) for line in lines})


# Each (column kind, option) where nanolance's layout is not yet pylance's, with the measured reason.
KNOWN_LAYOUT_GAPS = {
    ("float64", "lz4"): "pylance: InlineBitpacking(64) (its float default); nanolance's float default is Flat(64)",
    ("float64", "zstd"): "pylance: General{ZSTD,InlineBitpacking(64)}; nanolance's float default is Flat(64)",
}

# Layouts pylance writes that nanolance does not read yet (measured: General over InlineBitpacking).
KNOWN_READ_GAPS = {
    ("float64", "zstd"): "General{ZSTD,InlineBitpacking(64)} from pylance: the reader has no General over bitpacking",
}

CASES = [
    ("int64", "lz4"), ("int64", "zstd"), ("int64", "none"),
    ("int32", "lz4"), ("int32", "zstd"), ("int32", "none"),
    ("float64", "lz4"), ("float64", "zstd"), ("float64", "none"),
    ("float32", "none"),
    ("string", "lz4"), ("string", "zstd"), ("string", "none"),
    ("bool", "lz4"), ("bool", "none"),
    ("fsl", "lz4"), ("fsl", "none"),
]


@pytest.mark.parametrize(("kind", "option"), CASES)
def test_option_round_trips_and_pylance_reads_it(tmp_path, kind, option):
    lance = require_pylance()
    table = _table(kind, _opt("compression", option))
    path = tmp_path / "nl.lance"
    nl_write_dataset(table, str(path))
    assert lance.dataset(str(path)).to_table() == table
    # nanolance reads its own output too.
    assert nl_dataset(str(path)).to_table() == table


@pytest.mark.parametrize(("kind", "option"), CASES)
def test_nanolance_reads_what_pylance_writes_for_the_option(tmp_path, kind, option):
    lance = require_pylance()
    table = _table(kind, _opt("compression", option))
    path = tmp_path / "pylance.lance"
    lance.write_dataset(table, str(path))
    if (kind, option) in KNOWN_READ_GAPS:
        pytest.xfail(KNOWN_READ_GAPS[(kind, option)])
    assert nl_dataset(str(path)).to_table() == table


@pytest.mark.parametrize(("kind", "option"), CASES)
def test_layout_matches_pylance(tmp_path, kind, option):
    lance = require_pylance()
    table = _table(kind, _opt("compression", option))
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))
    if (kind, option) in KNOWN_LAYOUT_GAPS:
        pytest.xfail(KNOWN_LAYOUT_GAPS[(kind, option)])
    assert _layout(ours) == _layout(theirs)


@pytest.mark.parametrize("option", ["brotli", "LZ4"])
def test_unknown_scheme_is_refused_on_a_scalar_column(tmp_path, option):
    table = _table("int64", _opt("compression", option))
    with pytest.raises(Exception, match="compression"):
        nl_write_dataset(table, str(tmp_path / "bad.lance"))


def test_compression_level_is_accepted_and_ignored_by_lz4_and_by_unparsable_values(tmp_path):
    lance = require_pylance()
    for value in ["9", "abc"]:
        table = _table("int64", {**_opt("compression", "lz4"), **_opt("compression-level", value)})
        path = tmp_path / f"lvl-{value}.lance"
        nl_write_dataset(table, str(path))
        assert lance.dataset(str(path)).to_table() == table


def test_nested_columns_ignore_the_option_as_pylance_does(tmp_path):
    lance = require_pylance()
    # pylance accepts an unknown scheme on a list column (the option is not applied there).
    values = pa.array([[1, 2], [3], []], type=pa.list_(pa.int32()))
    table = pa.table({"c": values}, schema=pa.schema([pa.field("c", values.type, metadata=_opt("compression", "brotli"))]))
    path = tmp_path / "list.lance"
    nl_write_dataset(table, str(path))
    assert lance.dataset(str(path)).to_table() == table
