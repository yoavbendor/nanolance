"""W1t: a struct's nulls pushed into its children, as Lance's struct encoder does (`pushdown_nulls()`), and a layer's
kind decided by its whole bitmap (any null, even one under an outer null). Under a null struct pylance writes every
layer below it as nullable -- struct<x, s> with null structs is `layers=[nullable-item,nullable-item]` even where
the Arrow child has no null of its own, two definition bits where nanolance wrote one. The layout text (which shows
the definition width) equals pylance's; both readers read the files back."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pyarrow as pa
import pytest

from nanolance.lance.dataset import dataset as nl_dataset
from nanolance.lance.dataset import write_dataset as nl_write_dataset
from tests.support import require_pylance

PAGELAYOUT = Path(__file__).resolve().parents[3] / "build" / "nlance-pagelayout"


def _layout(path: Path) -> list[str]:
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    lines = {re.sub(r",rows=\d+", "", line.split("  ", 1)[1]) for line in out.splitlines()
             if "  " in line}
    # FSST is a size decision that differs on purpose (SPEC_GAPS W1g3).
    return sorted(re.sub(r"Fsst\{symbols=\d+B,values=(Variable\{offsets=Flat\(\d+\)\})\}", r"\1", x) for x in lines)


N = 3000
ST = pa.struct([("x", pa.int64()), ("s", pa.string())])


def _el(i):
    return {"x": i * 7919 % 100003, "s": f"value-{i * 7919 % 100003}"}


CASES = {
    # pyarrow puts a null under a null struct in x (int64) but not in s (string): s is still nullable in Lance.
    "struct": lambda: pa.array([None if i % 5 == 0 else _el(i) for i in range(N)], ST),
    "struct_from_arrays": lambda: pa.StructArray.from_arrays(
        [pa.array([_el(i)["x"] for i in range(N)]), pa.array([_el(i)["s"] for i in range(N)])], fields=list(ST),
        mask=pa.array([i % 5 == 0 for i in range(N)])),
    "struct_of_struct": lambda: pa.array([None if i % 5 == 0 else {"a": None if i % 7 == 0 else _el(i), "b": i}
                                          for i in range(N)],
                                         pa.struct([("a", ST), ("b", pa.int64())])),
    "inner_struct_only_null_under_outer": lambda: pa.StructArray.from_arrays(
        [pa.StructArray.from_arrays([pa.array(range(N))], names=["x"]), pa.array(range(N))], names=["a", "b"],
        mask=pa.array([i % 5 == 0 for i in range(N)])),
    "list_of_struct": lambda: pa.array([[None if (i + j) % 5 == 0 else _el(i + j) for j in range(i % 4)]
                                        for i in range(N)], pa.list_(ST)),
    "struct_of_list": lambda: pa.array([None if i % 5 == 0 else {"l": [i] * (i % 3), "b": i} for i in range(N)],
                                       pa.struct([("l", pa.list_(pa.int64())), ("b", pa.int64())])),
    "no_null_structs": lambda: pa.array([{"x": None if i % 3 == 0 else i, "s": f"v{i}"} for i in range(N)], ST),
}


@pytest.mark.parametrize("name", sorted(CASES))
def test_layers_under_a_null_struct_as_pylance(tmp_path, name):
    lance = require_pylance()
    table = pa.table({"v": CASES[name]()})
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))
    assert _layout(ours) == _layout(theirs)
    assert nl_dataset(str(ours)).to_table() == table
    assert lance.dataset(str(ours)).to_table() == table
    picks = [0, 1, 5, N // 2, N - 1]
    assert lance.dataset(str(ours)).take(picks) == table.take(picks)
    assert nl_dataset(str(ours)).take(picks) == table.take(picks)
