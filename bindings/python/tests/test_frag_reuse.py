"""Searching through the fragment-reuse index a deferred compaction writes.

pylance compacts with defer_index_remap=True: the indexes keep the row addresses they were built with,
and the __lance_frag_reuse system index maps them to the rewritten rows (src/frag_reuse.cpp). nanolance
reads such a dataset as pylance does: the index answers (its coverage moved to the new fragments, its
addresses are translated), the same rows come back, and the system index is listed. Each test indexes
a dataset with pylance, compacts it with pylance, and checks nanolance against pylance (rows and plan).

Vector and full-text indexes are not covered here: their searches still read a deferred compaction's
dataset through the stale coverage (an exact scan of the rewritten fragments), not through the index.
"""

from __future__ import annotations

import shutil
from pathlib import Path

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

N = 6_000
FILTERS = ["i = 600", "i < 1200", "i >= 5000", "i = 4321", "i IN (700, 2000, 9)"]


def _table() -> pa.Table:
    return pa.table({"i": range(N), "val": range(N), "tag": [f"t{x % 7}" for x in range(N)]})


def _rows(table: pa.Table) -> list[tuple]:
    return sorted(zip(table.column("i").to_pylist(), table.column("val").to_pylist()))


def _plan(path: str, filter_: str) -> str:
    return nl.dataset(path).scanner(filter=filter_).explain_plan()


def _check(lance, path: str, filters=FILTERS) -> None:
    """nanolance returns what pylance returns, and uses an index for the equality filters."""
    for filter_ in filters:
        expected = _rows(lance.dataset(path).to_table(filter=filter_))
        assert _rows(nl.dataset(path).to_table(filter=filter_)) == expected, filter_
    assert "ScalarIndexQuery" in _plan(path, "i = 4321")


def _deferred(lance, path: str, index: str, column: str = "i", kind: str = "BTREE") -> None:
    shutil.rmtree(path, ignore_errors=True)
    ds = lance.write_dataset(_table(), path, max_rows_per_file=1_000)
    ds.create_scalar_index(column, kind, name=index)
    ds.delete("i < 500")
    ds.optimize.compact_files(target_rows_per_fragment=2_000, defer_index_remap=True, num_threads=1)


@pytest.mark.parametrize("kind", ["BTREE", "BITMAP", "ZONEMAP", "BLOOMFILTER"])
def test_scalar_index_answers_through_frag_reuse(tmp_path: Path, kind: str):
    lance = require_pylance()
    path = str(tmp_path / "d.lance")
    _deferred(lance, path, "i_idx", kind=kind)
    assert any(i.name == "__lance_frag_reuse" for i in lance.dataset(path).describe_indices())
    _check(lance, path)


def test_bitmap_on_string_column_through_frag_reuse(tmp_path: Path):
    lance = require_pylance()
    path = str(tmp_path / "d.lance")
    _deferred(lance, path, "tag_idx", column="tag", kind="BITMAP")
    for filter_ in ["tag = 't3'", "tag IN ('t1', 't6')"]:
        expected = _rows(lance.dataset(path).to_table(filter=filter_))
        assert _rows(nl.dataset(path).to_table(filter=filter_)) == expected, filter_
    assert "ScalarIndexQuery" in _plan(path, "tag = 't3'")


def test_deletes_after_the_compaction(tmp_path: Path):
    lance = require_pylance()
    path = str(tmp_path / "d.lance")
    _deferred(lance, path, "i_idx")
    lance.dataset(path).delete("i >= 5900 and i < 5950")
    _check(lance, path)
    assert nl.dataset(path).count_rows() == N - 500 - 50


def test_two_deferred_compactions_in_a_row(tmp_path: Path):
    lance = require_pylance()
    path = str(tmp_path / "d.lance")
    _deferred(lance, path, "i_idx")
    ds = lance.dataset(path)
    ds.delete("i >= 5900 and i < 5950")
    ds.optimize.compact_files(target_rows_per_fragment=6_000, defer_index_remap=True, num_threads=1)
    assert len(lance.dataset(path).get_fragments()) == 1
    _check(lance, path)


def test_optimize_indices_after_frag_reuse_matches_pylance(tmp_path: Path):
    lance = require_pylance()
    path = str(tmp_path / "d.lance")
    other = str(tmp_path / "e.lance")
    _deferred(lance, path, "i_idx")
    shutil.copytree(path, other)
    lance.dataset(path).optimize.optimize_indices()
    nl.dataset(other).optimize.optimize_indices()
    for p in (path, other):
        listed = {i["name"]: (sorted(i["fragment_ids"]), i["version"]) for i in lance.dataset(p).list_indices()}
        assert listed["__lance_frag_reuse"][0] == [6, 7]
        assert listed["i_idx"][0] == [6, 7]
    assert {i["name"]: (sorted(i["fragment_ids"]), i["version"]) for i in nl.dataset(other).list_indices()} == {
        i["name"]: (sorted(i["fragment_ids"]), i["version"]) for i in lance.dataset(path).list_indices()}
    _check(lance, other)


def test_list_and_describe_show_the_system_index_as_pylance_does(tmp_path: Path):
    lance = require_pylance()
    path = str(tmp_path / "d.lance")
    _deferred(lance, path, "i_idx")
    expected = {i["name"]: (i["type"], sorted(i["fragment_ids"])) for i in lance.dataset(path).list_indices()}
    described = nl.dataset(path)
    listed = {i["name"]: (i["type"], sorted(i["fragment_ids"])) for i in described.list_indices()}
    assert listed == expected
    desc = {d.name: d for d in described.describe_indices()}
    assert {n: d.index_type for n, d in desc.items()} == {n: t for n, (t, _) in expected.items()}
    assert desc["__lance_frag_reuse"].fields == [] and desc["__lance_frag_reuse"].total_size_bytes is None
    pylance = {d.name: d for d in lance.dataset(path).describe_indices()}
    assert desc["i_idx"].num_rows_indexed == pylance["i_idx"].num_rows_indexed


# ── R2: nanolance writes the index (compact_files, and Compaction.plan / execute / commit) ────────────


def _nanolance_deferred(path: str, how: str = "compact_files") -> None:
    shutil.rmtree(path, ignore_errors=True)
    ds = nl.write_dataset(_table(), path, max_rows_per_file=1_000)
    ds.create_scalar_index("i", "BTREE")
    ds.delete("i < 500")
    if how == "compact_files":
        ds.optimize.compact_files(target_rows_per_fragment=2_000, defer_index_remap=True, num_threads=1)
        return
    from nanolance.lance.lance import Compaction

    plan = Compaction.plan(ds, options=dict(target_rows_per_fragment=2_000))
    results = [task.execute(ds) for task in plan.tasks]
    Compaction.commit(ds, results, options={"defer_index_remap": True})


def test_pylance_reads_the_index_nanolance_wrote(tmp_path: Path):
    lance = require_pylance()
    path = str(tmp_path / "nl.lance")
    _nanolance_deferred(path)
    described = {d.name: d.index_type for d in lance.dataset(path).describe_indices()}
    assert described.get("__lance_frag_reuse") == "FragmentReuse"
    _check(lance, path)  # pylance's rows, and nanolance's, through the index the nanolance compaction left


@pytest.mark.parametrize("how", ["compact_files", "plan"])
def test_plan_execute_commit_defers_as_compact_files_does(tmp_path: Path, how: str):
    lance = require_pylance()
    path = str(tmp_path / f"{how}.lance")
    _nanolance_deferred(path, how)
    assert {d.name for d in lance.dataset(path).describe_indices()} >= {"i_idx", "__lance_frag_reuse"}
    _check(lance, path)


def test_two_nanolance_deferred_compactions_in_a_row(tmp_path: Path):
    lance = require_pylance()
    path = str(tmp_path / "two.lance")
    _nanolance_deferred(path)
    ds = nl.dataset(path)
    ds.delete("i >= 5900")
    ds.optimize.compact_files(target_rows_per_fragment=6_000, defer_index_remap=True, num_threads=1)
    _check(lance, path, FILTERS + ["i = 5950"])


def test_eager_compaction_writes_no_frag_reuse_index(tmp_path: Path):
    lance = require_pylance()
    path = str(tmp_path / "eager.lance")
    shutil.rmtree(path, ignore_errors=True)
    ds = nl.write_dataset(_table(), path, max_rows_per_file=1_000)
    ds.create_scalar_index("i", "BTREE")
    ds.delete("i < 500")
    ds.optimize.compact_files(target_rows_per_fragment=2_000, num_threads=1)
    assert all(d.name != "__lance_frag_reuse" for d in nl.dataset(path).describe_indices())
    assert all(d.name != "__lance_frag_reuse" for d in lance.dataset(path).describe_indices())
    _check(lance, path)
