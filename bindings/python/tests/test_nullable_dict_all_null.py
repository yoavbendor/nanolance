"""Nullable low-cardinality strings written as dictionaries, and all-null columns written as Constant null pages.

Both match pylance 12 (measured on pylance 12.0.0): a string column with nulls is a dictionary whose nulls' empty
values are entries, with the definition levels in each chunk (`repdef=Bitpacked(16,Flat(1)),nullable`); an all-null
column stores no value at all (`Constant{nullable, all-null}`). Before, nanolance wrote every row plain and a zeroed
value slot per row: 8 bytes a row for an int64, ~40x pylance's size on a 70%-null string column.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

PAGELAYOUT = Path(__file__).resolve().parents[3] / "build" / "nlance-pagelayout"
N = 20_000
TOOLS = ["read_file", "edit_file", "glob_file_search", "run_terminal_command"]


def _layout(path):
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    # pylance wraps a string dictionary in General{LZ4}; nanolance keeps it plain by default (SPEC_GAPS W1g).
    return sorted(re.sub(r",rows=\d+", "", re.sub(r"General\{LZ4,(Variable\{[^}]*\})\}", r"\1", line.split("  ", 1)[1]))
                  for line in out.splitlines() if "  " in line)


def _mostly_null_strings(null_fraction, seed=3):
    import random
    rng = random.Random(seed)
    return pa.array([None if rng.random() < null_fraction else rng.choice(TOOLS) for _ in range(N)], pa.large_string())


CASES = {
    "mostly_null_strings": lambda: pa.table({"s": _mostly_null_strings(0.7)}),
    "half_null_strings": lambda: pa.table({"s": _mostly_null_strings(0.5, seed=4)}),
    "nulls_at_both_ends": lambda: pa.table({"s": pa.array([None] + [TOOLS[i % 4] for i in range(N - 2)] + [None],
                                                           pa.large_string())}),
    "all_null_int": lambda: pa.table({"a": pa.array([None] * N, pa.int64()), "b": pa.array(range(N), pa.int64())}),
    "all_null_string": lambda: pa.table({"s": pa.array([None] * N, pa.large_string())}),
    "all_null_float": lambda: pa.table({"f": pa.array([None] * N, pa.float64())}),
    "all_null_bool": lambda: pa.table({"flag": pa.array([None] * N, pa.bool_())}),
}


# Nulls in long runs: pylance run-length encodes the definition levels (Rle{Flat(16),Flat(8)}), nanolance bitpacks
# them -- the known gap SPEC_GAPS W1t2. The values read back the same both ways; only the layout text differs.
KNOWN_LEVEL_RLE_GAP = {"nulls_at_both_ends"}


@pytest.mark.parametrize("name", sorted(CASES))
def test_written_as_pylance_and_read_both_ways(tmp_path, name):
    lance = require_pylance()
    table = CASES[name]()
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl.write_dataset(table, str(ours), mode="overwrite")
    lance.write_dataset(table, str(theirs), mode="overwrite")
    if name not in KNOWN_LEVEL_RLE_GAP:
        assert _layout(ours) == _layout(theirs), (_layout(ours), _layout(theirs))
    assert nl.dataset(str(ours)).to_table().equals(table)
    assert lance.dataset(str(ours)).to_table().equals(table)
    assert nl.dataset(str(theirs)).to_table().equals(table)
    picks = [0, 1, 5, N // 2, N - 1]
    assert nl.dataset(str(ours)).take(picks).equals(table.take(picks))
    assert lance.dataset(str(ours)).take(picks).equals(table.take(picks))


def test_nullable_dictionary_filters_and_appends(tmp_path):
    lance = require_pylance()
    base = pa.table({"id": pa.array(range(N), pa.int64()), "s": _mostly_null_strings(0.6, seed=9)})
    path = tmp_path / "ds.lance"
    nl.write_dataset(base.slice(0, N // 2), str(path), mode="overwrite")
    nl.write_dataset(base.slice(N // 2), str(path), mode="append")
    assert nl.dataset(str(path)).to_table().equals(base)
    assert lance.dataset(str(path)).to_table().equals(base)
    got = nl.dataset(str(path)).to_table(filter="s = 'read_file'")
    want = base.filter(pa.compute.equal(base["s"], "read_file"))
    assert got.num_rows == want.num_rows and got["id"].to_pylist() == want["id"].to_pylist()
    assert nl.dataset(str(path)).to_table(filter="s IS NULL").num_rows == base["s"].null_count


def test_all_null_column_beside_other_columns_on_s3_shape(tmp_path):
    """The chunk shape: several all-null columns beside ordinary ones, in one fragment and several."""
    lance = require_pylance()
    table = pa.table({
        "id": pa.array(range(N), pa.int64()),
        "session_created_at": pa.array([None] * N, pa.int64()),
        "tool_name": _mostly_null_strings(0.4, seed=5),
        "exit_code": pa.array([None] * N, pa.int32()),
    })
    path = tmp_path / "shape.lance"
    nl.write_dataset(table.slice(0, N // 2), str(path), mode="overwrite", max_rows_per_file=5_000)
    nl.write_dataset(table.slice(N // 2), str(path), mode="append", max_rows_per_file=5_000)
    assert nl.dataset(str(path)).to_table().equals(table)
    assert lance.dataset(str(path)).to_table().equals(table)
