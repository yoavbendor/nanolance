"""scan_stats_callback: one ScanStatistics per scan, with pylance's attribute names, and index cache
hits and misses that follow the index cache.

What has to hold: the callback fires once per to_table() (and so per to_batches() / to_reader()), with
pylance's attributes and types; a first query on an index misses and a repeat on the same dataset object
hits, for BTREE, BITMAP, INVERTED (full-text) and phrase queries; a value no index holds reads nothing and
counts nothing; a scan without an index reports zero index cache lookups.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance.lance as lance

ATTRIBUTES = {
    "all_counts": dict,
    "bytes_read": int,
    "index_cache_hits": int,
    "index_cache_misses": int,
    "index_comparisons": int,
    "indices_loaded": int,
    "iops": int,
    "parts_loaded": int,
    "requests": int,
}


class Recorder:
    def __init__(self):
        self.calls = []

    def __call__(self, stats):
        self.calls.append(stats)

    @property
    def last(self):
        assert self.calls, "scan_stats_callback was not called"
        return self.calls[-1]


def scan(ds, recorder, **kwargs):
    ds.scanner(scan_stats_callback=recorder, **kwargs).to_table()
    assert len(recorder.calls) >= 1
    return recorder.last


def test_callback_fires_once_per_scan_with_pylance_attributes(tmp_path):
    lance.write_dataset(pa.table({"val": list(range(1000))}), tmp_path / "ds").create_scalar_index("val", "BTREE")
    ds = lance.dataset(tmp_path / "ds")
    rec = Recorder()
    ds.scanner(filter="val = 42", scan_stats_callback=rec).to_table()
    assert len(rec.calls) == 1
    stats = rec.last
    assert type(stats).__name__ == "ScanStatistics"
    for name, kind in ATTRIBUTES.items():
        assert isinstance(getattr(stats, name), kind), name
    assert stats.index_cache_misses + stats.index_cache_hits == 1


def test_callback_fires_once_for_batches_and_reader(tmp_path):
    lance.write_dataset(pa.table({"val": list(range(1000))}), tmp_path / "ds").create_scalar_index("val", "BTREE")
    ds = lance.dataset(tmp_path / "ds")
    rec = Recorder()
    list(ds.scanner(filter="val = 42", scan_stats_callback=rec).to_batches())
    assert len(rec.calls) == 1
    ds.scanner(filter="val = 42", scan_stats_callback=rec).to_reader().read_all()
    assert len(rec.calls) == 2


def test_no_callback_no_reports(tmp_path):
    lance.write_dataset(pa.table({"val": list(range(10))}), tmp_path / "ds")
    ds = lance.dataset(tmp_path / "ds")
    assert ds.scanner(filter="val = 3").to_table().num_rows == 1


def test_scan_without_index_has_no_index_lookups(tmp_path):
    lance.write_dataset(pa.table({"val": list(range(1000))}), tmp_path / "ds")
    stats = scan(lance.dataset(tmp_path / "ds"), Recorder(), filter="val = 42")
    assert stats.index_cache_hits == 0
    assert stats.index_cache_misses == 0


def test_btree_first_query_misses_repeat_hits(tmp_path):
    lance.write_dataset(pa.table({"val": list(range(1000))}), tmp_path / "ds").create_scalar_index("val", "BTREE")
    ds = lance.dataset(tmp_path / "ds")
    rec = Recorder()
    first = scan(ds, rec, filter="val = 42")
    assert (first.index_cache_misses, first.index_cache_hits) == (1, 0)
    repeat = scan(ds, rec, filter="val = 42")
    assert (repeat.index_cache_misses, repeat.index_cache_hits) == (0, 1)


def test_bitmap_counts_each_value_found_and_nothing_for_absent_value(tmp_path):
    lance.write_dataset(pa.table({"color": ["red", "green", "blue", "yellow"] * 25}),
                        tmp_path / "ds").create_scalar_index("color", "BITMAP")
    ds = lance.dataset(tmp_path / "ds")
    rec = Recorder()
    first = scan(ds, rec, filter="color IN ('red', 'blue')")
    assert (first.index_cache_misses, first.index_cache_hits) == (2, 0)
    repeat = scan(ds, rec, filter="color IN ('red', 'blue')")
    assert (repeat.index_cache_misses, repeat.index_cache_hits) == (0, 2)
    absent = scan(ds, rec, filter="color = 'purple'")
    assert (absent.index_cache_misses, absent.index_cache_hits) == (0, 0)


def test_fts_first_query_misses_repeat_hits(tmp_path):
    lance.write_dataset(pa.table({"text": ["word"] * 100}), tmp_path / "ds").create_scalar_index("text", "INVERTED")
    ds = lance.dataset(tmp_path / "ds")
    rec = Recorder()
    first = scan(ds, rec, full_text_query="word")
    assert first.index_cache_misses > 0
    assert first.index_cache_misses > first.index_cache_hits
    repeat = scan(ds, rec, full_text_query="word")
    assert repeat.index_cache_hits > 0
    assert repeat.index_cache_misses == 0


def test_phrase_query_first_query_misses_repeat_hits(tmp_path):
    lance.write_dataset(pa.table({"text": ["quick brown fox jumps over lazy dog"] * 50}),
                        tmp_path / "ds").create_scalar_index("text", "INVERTED", with_position=True)
    ds = lance.dataset(tmp_path / "ds")
    rec = Recorder()
    first = scan(ds, rec, full_text_query='"quick brown"')
    assert first.index_cache_misses > first.index_cache_hits
    repeat = scan(ds, rec, full_text_query='"quick brown"')
    assert repeat.index_cache_hits > 0
    assert repeat.index_cache_misses == 0


def test_bytes_and_requests_follow_the_reads(tmp_path):
    lance.write_dataset(pa.table({"val": list(range(1000))}), tmp_path / "ds").create_scalar_index("val", "BTREE")
    stats = scan(lance.dataset(tmp_path / "ds"), Recorder(), filter="val = 42")
    assert stats.iops > 0
    assert stats.requests == stats.iops
    assert stats.bytes_read > 0
