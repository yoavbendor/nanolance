# SPDX-License-Identifier: Apache-2.0
"""Index coverage after compaction, as Lance leaves it: an index takes back the fragments that replaced ones
it covered, never an unindexed fragment beside them; a zone map is not remapped (optimize_indices brings it
back); and remap_row_addrs follows rows across a deferred-remap compaction."""

import pyarrow as pa

from tests.support import require_pylance

import nanolance.lance as nl


def _coverage(ds, name):
    return sorted(i for seg in next(d for d in ds.describe_indices() if d.name == name).segments
                  for i in seg.fragment_ids)


def test_compaction_does_not_index_unindexed_fragments(tmp_path):
    lance = require_pylance()
    path = str(tmp_path / "ds")
    t = lambda lo: pa.table({"id": pa.array(range(lo, lo + 300), pa.int64())})
    ds = nl.write_dataset(t(0), path, max_rows_per_file=150)
    ds.create_scalar_index("id", "BTREE")
    ds = nl.write_dataset(t(300), path, mode="append", max_rows_per_file=150)
    ds.delete("id % 4 == 0")
    ds.optimize.compact_files()
    ds = nl.dataset(path)
    assert len(ds.get_fragments()) == 2
    covered = _coverage(ds, "id_idx")
    assert len(covered) == 1 and covered[0] in [f.fragment_id for f in ds.get_fragments()]
    assert ds.to_table(filter="id < 300").num_rows == 225
    assert lance.dataset(path).to_table(filter="id < 300").num_rows == 225


def test_zone_map_left_to_optimize_indices(tmp_path):
    path = str(tmp_path / "ds")
    ds = nl.write_dataset(pa.table({"v": range(5000)}), path, max_rows_per_file=1000)
    ds.create_scalar_index("v", index_type="ZONEMAP")
    ds.optimize.compact_files(target_rows_per_fragment=2000)
    assert "ScalarIndexQuery" not in ds.scanner(filter="v > 2500", prefilter=True).explain_plan()
    assert ds.to_table(filter="v > 2500").num_rows == 2499
    ds.optimize.optimize_indices()
    assert "ScalarIndexQuery" in ds.scanner(filter="v > 2500", prefilter=True).explain_plan()
    assert ds.to_table(filter="v > 2500").num_rows == 2499


def test_remap_row_addrs(tmp_path):
    path = str(tmp_path / "ds")
    ds = nl.write_dataset(pa.table({"id": range(1000)}), path, max_rows_per_file=100)
    assert ds.remap_row_addrs(pa.array([0, 1 << 32], pa.uint64())) is None
    before = ds.to_table(columns=["id"], with_row_address=True)
    ds.create_scalar_index("id", "BTREE")
    ds.delete("id = 5")
    ds.optimize.compact_files(target_rows_per_fragment=1000, defer_index_remap=True)
    ds = nl.dataset(path)
    after = ds.to_table(columns=["id"], with_row_address=True)
    new = dict(zip(after["id"].to_pylist(), after["_rowaddr"].to_pylist()))
    old = dict(zip(before["id"].to_pylist(), before["_rowaddr"].to_pylist()))
    got = ds.remap_row_addrs(pa.array([old[i] for i in (0, 5, 137, 999)], pa.uint64())).to_pylist()
    assert got == [new[0], None, new[137], new[999]]
