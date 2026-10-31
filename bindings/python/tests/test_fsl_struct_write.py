"""fixed_size_list<struct, N> written as pylance 12 writes it: one column per struct leaf, rows x N entries each, with
a definition layer for the list (each row's validity standing for its N elements), the struct and the leaf. Both
readers read it back, and the layout text equals pylance's (leaving out FSST on strings, which nanolance keeps only when
it is at least 10% smaller, and page row counts, cut as for every nested leaf)."""

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
ST = pa.struct([("x", pa.int64()), ("s", pa.string())])


def _layout(path: Path) -> list[str]:
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    lines = {re.sub(r",rows=\d+", "", line.split("  ", 1)[1]) for line in out.splitlines() if "  " in line}
    # FSST is a size decision that differs on purpose (see the module docstring).
    return sorted(re.sub(r"Fsst\{symbols=\d+B,values=(Variable\{offsets=Flat\(\d+\)\})\}", r"\1", x) for x in lines)


def _rows(rows, n=2, null_row=0, null_struct=0, null_x=0, start=0):
    out = []
    for i in range(start, start + rows):
        if null_row and i % null_row == 0:
            out.append(None)
            continue
        out.append([None if null_struct and (n * i + j) % null_struct == 0
                    else {"x": None if null_x and (n * i + j) % null_x == 0 else n * i + j, "s": f"s{n * i + j}"}
                    for j in range(n)])
    return out


CASES = {
    "plain": lambda: pa.table({"v": pa.array(_rows(3000), pa.list_(ST, 2))}),
    "null_rows": lambda: pa.table({"v": pa.array(_rows(3000, null_row=7), pa.list_(ST, 2))}),
    "null_structs": lambda: pa.table({"v": pa.array(_rows(3000, null_struct=5), pa.list_(ST, 2))}),
    "null_leaves": lambda: pa.table({"v": pa.array(_rows(3000, null_x=3), pa.list_(ST, 2))}),
    "all_nulls": lambda: pa.table({"v": pa.array(_rows(3000, null_row=7, null_struct=5, null_x=3), pa.list_(ST, 2))}),
    "n5": lambda: pa.table({"v": pa.array(_rows(2000, n=5, null_row=4, null_x=7), pa.list_(ST, 5))}),
    "first_row_null": lambda: pa.table({"v": pa.array([None] + _rows(10, start=1), pa.list_(ST, 2))}),
    "many_pages": lambda: pa.table({"id": pa.array(range(200_000)),
                                    "v": pa.array(_rows(200_000, null_row=7, null_struct=5, null_x=3),
                                                  pa.list_(ST, 2))}),
}


# Cases whose layout text equals pylance's. Under a null struct Lance pushes the struct's nulls into its children, so
# every leaf below it is a nullable layer; nanolance does not do that yet (see SPEC_GAPS W1t).
LAYOUT_SAME = {"plain", "null_leaves"}


@pytest.mark.parametrize("name", sorted(CASES))
def test_fsl_struct_written_as_pylance(tmp_path, name):
    lance = require_pylance()
    table = CASES[name]()
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))
    if name in LAYOUT_SAME:
        assert _layout(ours) == _layout(theirs)
    assert nl_dataset(str(ours)).to_table() == table
    # Lance 12 cannot read a fixed_size_list<struct> column together with another column (it counts the struct's
    # leaves, rows x N, against the other columns' rows -- also on files it wrote itself), so pylance reads "v" alone.
    v = table.select(["v"])
    assert lance.dataset(str(ours)).to_table(columns=["v"]) == v
    picks = [0, 1, table.num_rows // 2, table.num_rows - 1]
    assert lance.dataset(str(ours)).take(picks, columns=["v"]) == v.take(picks)
    assert nl_dataset(str(ours)).take(picks) == table.take(picks)


def test_pylance_cannot_read_it_with_another_column(tmp_path):
    """Pins the Lance 12 limit the test above works around: on pylance's own file too."""
    lance = require_pylance()
    table = pa.table({"id": pa.array(range(100)), "v": pa.array(_rows(100), pa.list_(ST, 2))})
    theirs = tmp_path / "theirs.lance"
    lance.write_dataset(table, str(theirs))
    with pytest.raises(Exception, match="differing lengths"):
        lance.dataset(str(theirs)).to_table()
    ours = tmp_path / "ours.lance"
    nl_write_dataset(table, str(ours))
    assert nl_dataset(str(ours)).to_table() == table


@pytest.mark.parametrize("column", [
    pa.array([{"a": [{"x": 1}, {"x": 2}]}], pa.struct([("a", pa.list_(pa.struct([("x", pa.int64())]), 2))])),
    pa.array([[[{"x": 1}, {"x": 2}]]], pa.list_(pa.list_(pa.struct([("x", pa.int64())]), 2))),
    pa.array([[{"y": [1]}, {"y": []}]], pa.list_(pa.struct([("y", pa.list_(pa.int32()))]), 2)),
], ids=["in_struct", "in_list", "list_under_it"])
def test_shapes_not_written_are_refused(tmp_path, column):
    with pytest.raises(Exception, match="fixed_size_list"):
        nl_write_dataset(pa.table({"v": column}), str(tmp_path / "t.lance"))


def test_a_null_row_in_a_later_batch(tmp_path):
    """The list layer's validity is materialized at its first null, which may come several batches in."""
    lance = require_pylance()
    schema = pa.schema([("v", pa.list_(ST, 2))])
    batches = [pa.record_batch([pa.array(_rows(500, start=0), pa.list_(ST, 2))], schema=schema),
               pa.record_batch([pa.array(_rows(500, null_row=3, start=500), pa.list_(ST, 2))], schema=schema)]
    table = pa.Table.from_batches(batches)
    ours = tmp_path / "ours.lance"
    nl_write_dataset(pa.RecordBatchReader.from_batches(schema, batches), str(ours), schema=schema)
    assert nl_dataset(str(ours)).to_table() == table
    assert lance.dataset(str(ours)).to_table() == table


def test_a_sliced_column(tmp_path):
    lance = require_pylance()
    whole = pa.array(_rows(3000, null_row=7, null_struct=5, null_x=3), pa.list_(ST, 2))
    table = pa.table({"v": whole.slice(101, 1700)})
    ours = tmp_path / "ours.lance"
    nl_write_dataset(table, str(ours))
    assert nl_dataset(str(ours)).to_table() == table
    assert lance.dataset(str(ours)).to_table() == table


def test_appends_and_filters(tmp_path):
    table = pa.table({"id": pa.array(range(1000)), "v": pa.array(_rows(1000, null_row=7, null_x=3), pa.list_(ST, 2))})
    ours = tmp_path / "ours.lance"
    nl_write_dataset(table.slice(0, 600), str(ours))
    nl_write_dataset(table.slice(600), str(ours), mode="append")
    ds = nl_dataset(str(ours))
    assert ds.to_table() == table
    assert ds.to_table(filter="id >= 990") == table.slice(990)
