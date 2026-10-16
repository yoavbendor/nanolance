"""Lance's system columns, against pylance.

``_rowid``, ``_rowaddr``, ``_rowoffset`` and the row version columns
(``_row_created_at_version``, ``_row_last_updated_at_version``) in scans and takes, where the
projection names them; filters on ``_rowid`` / ``_rowaddr`` (with limit and offset after the
filter); ``_distance`` after the named columns unless ``disable_scoring_autoprojection``; a
dataset opened with ``default_scan_options`` showing the row id columns in its schema.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


def _pair(tmp_path):
    lance = require_pylance()
    paths = []
    for lib, name in ((nl, "n"), (lance, "p")):
        path = str(tmp_path / f"{name}.lance")
        ds = lib.write_dataset(pa.table({"a": range(100), "b": range(100, 200)}), path, max_rows_per_file=25)
        ds.delete("a % 7 = 3")
        lib.dataset(path).insert(pa.table({"a": [500, 501], "b": [1, 2]}))
        paths.append(path)
    return lance, paths


SYSTEM = ["_rowid", "_rowaddr", "_rowoffset", "_row_created_at_version", "_row_last_updated_at_version"]


@pytest.mark.parametrize("columns", [
    ["_rowoffset", "a"], ["a", "_row_created_at_version", "_rowid"], ["_row_last_updated_at_version"],
    ["_rowaddr", "_rowid", "b", "a"],
])
def test_take(tmp_path, columns):
    lance, (ours, theirs) = _pair(tmp_path)
    indices = [0, 5, 30, 61, 87]
    for path in (ours, theirs):
        assert nl.dataset(path).take(indices, columns=columns).equals(
            lance.dataset(path).take(indices, columns=columns))


@pytest.mark.parametrize("filter", [
    "_rowid = 4294967298", "_rowaddr >= 8589934592 AND a < 60", "_rowid IN (1, 2, 30064771072) OR b = 150",
])
@pytest.mark.parametrize("limit,offset", [(None, None), (3, 1)])
def test_filter_on_row_ids(tmp_path, filter, limit, offset):
    lance, (_, path) = _pair(tmp_path)
    kw = dict(filter=filter, limit=limit, offset=offset)
    for extra in ({}, {"with_row_id": True}, {"columns": ["b"]}, {"columns": ["b"], "with_row_address": True}):
        assert nl.dataset(path).to_table(**kw, **extra).equals(lance.dataset(path).to_table(**kw, **extra))
    # (pylance's own count_rows fails on some of these filters; its table's length is the count.)
    assert nl.dataset(path).count_rows(filter=filter) == lance.dataset(path).to_table(filter=filter).num_rows


def test_scan_projection_with_system_columns(tmp_path):
    lance, (_, path) = _pair(tmp_path)
    cols = ["_row_created_at_version", "a", "_rowoffset", "_rowid"]
    assert nl.dataset(path).to_table(columns=cols).equals(lance.dataset(path).to_table(columns=cols))


def test_distance_placement(tmp_path):
    lance = require_pylance()
    data = pa.table({"vec": pa.array([[i, i] for i in range(50)], pa.list_(pa.float32(), 2)), "x": range(50)})
    names = []
    for lib, name in ((nl, "n"), (lance, "p")):
        ds = lib.write_dataset(data, str(tmp_path / f"{name}.lance"))
        q = {"column": "vec", "q": pa.array([1, 1], pa.float32()), "k": 5, "use_index": False}
        names.append([
            ds.scanner(nearest=q, columns=c, disable_scoring_autoprojection=d, **kw).to_table().schema.names
            for c, d, kw in ((None, None, {}), (None, None, {"with_row_id": True}), (["_rowid", "vec"], False, {}),
                             (["vec"], True, {}), (["vec", "_distance", "x"], True, {}))
        ])
    assert names[0] == names[1]


def test_default_scan_options_schema(tmp_path):
    path = str(tmp_path / "d.lance")
    nl.write_dataset(pa.table({"x": [0, 1]}), path)
    ds = nl.dataset(path, default_scan_options={"with_row_id": True})
    assert ds.schema.names == ["x", "_rowid"]
    assert ds.schema == ds.to_table().schema
    nl.dataset(path, default_scan_options={"with_row_id": True}).insert(pa.table({"x": [2]}))
    assert nl.dataset(path).count_rows() == 3


def test_batch_size_bytes(tmp_path):
    path = str(tmp_path / "b.lance")
    nl.write_dataset(pa.table({"t": pa.array(["x" * 10240] * 200, pa.large_string())}), path)
    batches = list(nl.dataset(path).to_batches(batch_size_bytes=50 * 1024))
    assert sum(b.num_rows for b in batches) == 200 and len(batches) > 10
