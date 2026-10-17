"""LanceDataset.io_stats_incremental(): pylance's file I/O counters, per call since the last call.

What has to hold: the attribute names and types are pylance's; a count answered from a scalar index
reads nothing on the repeat call (the index and the deletion files are kept by the dataset object);
a scan or a take reads and reports bytes; each call returns what was done since the previous one
(so a call right after another reads zero); and a delete (a write) reports written bytes.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance.lance as lance

ATTRIBUTES = ("read_iops", "read_bytes", "write_iops", "written_bytes")


@pytest.fixture
def dataset(tmp_path):
    table = pa.table({"x": pa.array(range(100), pa.int64()), "s": pa.array([f"v{i}" for i in range(100)])})
    lance.write_dataset(table, tmp_path / "ds", max_rows_per_file=25)
    ds = lance.dataset(tmp_path / "ds")
    ds.create_scalar_index("x", "BTREE")
    ds.io_stats_incremental()  # start from zero
    return ds


def _stats(ds):
    stats = ds.io_stats_incremental()
    assert all(isinstance(getattr(stats, name), int) for name in ATTRIBUTES), stats
    return stats


def test_attributes_are_pylances(dataset):
    stats = _stats(dataset)
    assert [name for name in ATTRIBUTES if hasattr(stats, name)] == list(ATTRIBUTES)


def test_index_answered_count_reads_nothing_on_repeat(dataset):
    assert dataset.count_rows(filter="x < 50") == 50
    _stats(dataset)  # the first count is not the one under test
    assert dataset.count_rows(filter="x < 50") == 50
    stats = _stats(dataset)
    assert (stats.read_iops, stats.read_bytes, stats.write_iops, stats.written_bytes) == (0, 0, 0, 0)


def test_scan_and_take_read_and_report_bytes(dataset):
    dataset.to_table()
    stats = _stats(dataset)
    assert stats.read_iops > 0 and stats.read_bytes > 0
    assert stats.write_iops == 0 and stats.written_bytes == 0

    assert dataset.take([1, 2, 3]).num_rows == 3
    stats = _stats(dataset)
    assert stats.read_iops > 0 and stats.read_bytes > 0


def test_each_call_reports_only_what_came_since_the_last(dataset):
    dataset.to_table()
    first = _stats(dataset)
    assert first.read_iops > 0
    assert _stats(dataset).read_iops == 0
    dataset.to_table()
    again = _stats(dataset)
    # The same scan reads again (pylance does not cache data reads either). Not necessarily the same
    # bytes: each reader thread keeps a file's end and its last small block, so how much a repeat
    # reads depends on which thread reads which file.
    assert again.read_iops > 0 and again.read_bytes > 0


def test_a_write_reports_written_bytes(dataset):
    dataset.delete("x = 10")
    stats = _stats(dataset)
    assert stats.write_iops > 0 and stats.written_bytes > 0


def test_a_fresh_object_starts_at_zero(tmp_path, dataset):
    fresh = lance.dataset(dataset.uri)
    stats = fresh.io_stats_incremental()
    assert (stats.read_iops, stats.read_bytes, stats.write_iops, stats.written_bytes) == (0, 0, 0, 0)


def test_count_from_index_follows_the_dataset_to_a_new_version(tmp_path):
    ds = lance.write_dataset(pa.table({"x": pa.array(range(100))}), str(tmp_path / "a"), max_rows_per_file=25)
    ds.create_scalar_index("x", "BTREE")
    assert ds.count_rows(filter="x < 50") == 50
    ds.delete("x < 10")
    assert ds.count_rows(filter="x < 50") == 40
    ds.insert(pa.table({"x": pa.array([1, 2, 3])}))
    assert ds.count_rows(filter="x < 50") == 43
