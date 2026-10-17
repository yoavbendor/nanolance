"""Count pushdown from scalar indices: count_rows(filter) answered from an exact index's mask.

Both engines run side by side on the same dataset: nanolance's count must equal pylance's and a
pyarrow count over pylance's full read. The plan must name CountFromMask only where an exact index
answers the filter; everything else (inexact indices, unindexed columns, uncovered fragments) scans.
"""

from __future__ import annotations

import pyarrow as pa
import pyarrow.compute as pc
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

ROWS_PER_FRAGMENT = 25
NUM_ROWS = 100

# (filter, pyarrow mask over the table's x and y columns)
FILTERS = [
    ("x < 50", lambda t: pc.less(t["x"], 50)),
    ("x = 7", lambda t: pc.equal(t["x"], 7)),
    ("x >= 90", lambda t: pc.greater_equal(t["x"], 90)),
    ("x < 50 AND x > 10", lambda t: pc.and_(pc.less(t["x"], 50), pc.greater(t["x"], 10))),
    ("x < 10 OR x > 90", lambda t: pc.or_(pc.less(t["x"], 10), pc.greater(t["x"], 90))),
    ("x < 50 AND y = 3", lambda t: pc.and_(pc.less(t["x"], 50), pc.equal(t["y"], 3))),
]


def _table(lo: int, hi: int) -> pa.Table:
    x = pa.array(range(lo, hi), pa.int64())
    return pa.table({"x": x, "y": pa.array([v % 7 for v in range(lo, hi)], pa.int64())})


def _make(tmp_path, *, index_type: str | None = "BTREE", column: str = "x"):
    path = tmp_path / "ds"
    ds = nl.write_dataset(_table(0, NUM_ROWS), str(path), max_rows_per_file=ROWS_PER_FRAGMENT)
    if index_type is not None:
        ds.create_scalar_index(column, index_type)
    return path, ds


def _expected(path, sql: str, mask) -> int:
    table = require_pylance().dataset(str(path)).to_table()
    return table.filter(mask(table)).num_rows


def _assert_counts(path, ds, cases=FILTERS):
    lance = require_pylance()
    pyds = lance.dataset(str(path))
    for sql, mask in cases:
        expected = _expected(path, sql, mask)
        assert pyds.count_rows(filter=sql) == expected, sql
        assert ds.count_rows(filter=sql) == expected, sql


@pytest.mark.parametrize("index_type", ["BTREE", "BITMAP"])
def test_exact_index_counts_match_pylance(tmp_path, index_type):
    path, ds = _make(tmp_path, index_type=index_type)
    _assert_counts(path, ds)


@pytest.mark.parametrize("index_type", ["BTREE", "BITMAP"])
def test_counts_after_deletes_updates_and_whole_fragment_delete(tmp_path, index_type):
    path, ds = _make(tmp_path, index_type=index_type)
    ds.delete("x = 10 OR x = 20 OR x = 30")
    ds.update({"x": "100"}, where="x = 5")
    ds.update({"x": "8"}, where="x = 60")
    ds.delete("x >= 75 AND x < 100")  # the whole last fragment
    _assert_counts(path, ds)


def test_partial_coverage_counts_scan_the_uncovered_fragment(tmp_path):
    path, ds = _make(tmp_path)
    nl.write_dataset(_table(NUM_ROWS, NUM_ROWS + 20), str(path), mode="append")
    ds = nl.dataset(str(path))  # the index covers the first four fragments only
    _assert_counts(path, ds)
    plan = ds.scanner(columns=[], with_row_id=True, filter="x < 50").analyze_plan(count_rows=True)
    assert "CountFromMask" in plan and "UnionExec" in plan and "LanceRead" in plan, plan


@pytest.mark.parametrize("index_type", ["ZONEMAP", "BLOOMFILTER"])
def test_inexact_index_counts_correct_and_scan(tmp_path, index_type):
    path, ds = _make(tmp_path, index_type=index_type)
    _assert_counts(path, ds)
    plan = ds.scanner(columns=[], with_row_id=True, filter="x = 7").analyze_plan(count_rows=True)
    assert "CountFromMask" not in plan, plan


def test_unindexed_column_in_filter_falls_back(tmp_path):
    path, ds = _make(tmp_path)
    _assert_counts(path, ds, [FILTERS[5]])
    plan = ds.scanner(columns=[], with_row_id=True, filter="x < 50 AND y = 3").analyze_plan(count_rows=True)
    assert "CountFromMask" not in plan and "LanceRead" in plan, plan


def test_plan_names_count_from_mask_without_reading_columns(tmp_path):
    path, ds = _make(tmp_path)
    ds.delete("x = 10")
    plan = ds.scanner(columns=[], with_row_id=True, filter="x < 50").analyze_plan(count_rows=True)
    assert "CountFromMask" in plan and "ScalarIndexQuery" in plan, plan
    assert "LanceRead" not in plan, plan
    assert ds.count_rows(filter="x < 50") == 49
