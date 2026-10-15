"""ZONEMAP scalar indexes: nanolance.lance against pylance on the same data and operations.

A zonemap is a file of zones (min, max, null and NaN counts, the fragment and row span of each zone)
and the null rows. Built by either engine it must read the same to the other: the same rows,
the same zone table, and a plan that names the index. Maintenance (append, delete, update,
compaction, optimize) keeps the results the same on both engines.
"""

from __future__ import annotations

import glob
import os
import shutil

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

pytestmark = pytest.mark.filterwarnings("ignore:The 'list_indices' method is deprecated")

FILTERS = [
    "id = 5000", "id = 9999", "id BETWEEN 100 AND 180", "id > 9990", "id <= 3", "id IN (7, 4321, 9000)",
    "v IS NULL", "v IS NOT NULL", "v = 97", "v >= 9000", "f = 5.0", "f > 19990", "f < 2",
    "s = 'k0100'", "s >= 'k9990'", "b = true", "b IS NULL",
]


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _table(n=10000, start=0):
    ids = list(range(start, start + n))
    return pa.table({
        "id": pa.array(ids, pa.int64()),
        "v": pa.array([None if i % 97 == 0 else i for i in ids], pa.int64()),
        "f": pa.array([None if i % 211 == 0 else (float("nan") if i % 401 == 0 else float(i)) for i in ids],
                      pa.float64()),
        "s": pa.array([None if i % 89 == 0 else f"k{i:04d}" for i in ids], pa.string()),
        "b": pa.array([None if i % 53 == 0 else i % 2 == 0 for i in ids], pa.bool_()),
    })


def _fresh(tmp_path, name):
    path = str(tmp_path / name)
    shutil.rmtree(path, ignore_errors=True)
    return path


def _zone_files(path):
    return sorted(glob.glob(os.path.join(path, "_indices", "*", "zonemap.lance")))


def _zone_table(lance, path):
    from lance.file import LanceFileReader

    (file,) = _zone_files(path)
    reader = LanceFileReader(file)
    return reader.read_all().to_table(), {k.decode(): v.decode() for k, v in reader.metadata().schema.metadata.items()
                                          if k in (b"rows_per_zone", b"null_bitmap")}


def _ids(table):
    return sorted(table.column("id").to_pylist())


def _both_build(lance, tmp_path, columns, rows=10000, fragments=2, kind="ZONEMAP"):
    """The same dataset written twice, each with an index of `kind` on every column built by one engine."""
    out = {}
    prefix = "zm" if kind == "ZONEMAP" else "bf"
    for engine in ("pylance", "nanolance"):
        path = _fresh(tmp_path, f"{engine}.lance")
        module = lance if engine == "pylance" else nl
        table = _table(rows)
        step = rows // fragments
        module.write_dataset(table.slice(0, step), path)
        for k in range(1, fragments):
            module.write_dataset(table.slice(k * step, step), path, mode="append")
        for column in columns:
            module.dataset(path).create_scalar_index(column, kind, name=f"{prefix}_{column}")
        out[engine] = path
    return out


def _bloom_table(module, path, name):
    """The bloomfilter.lance of the index `name` (found by its uuid in the listing), and its metadata."""
    from lance.file import LanceFileReader

    uuid = next(i["uuid"] for i in module.dataset(path).list_indices() if i["name"] == name)
    reader = LanceFileReader(os.path.join(path, "_indices", uuid, "bloomfilter.lance"))
    return reader.read_all().to_table(), {k.decode(): v.decode() for k, v in reader.metadata().schema.metadata.items()}


FILTERS_BLOOM = [
    "id = 5000", "id = 9999", "id = 123456", "id IN (7, 4321, 9000)", "id IN (-1)", "v = 97", "v IS NULL",
    "f = 5.0", "f = 0", "f = 211", "f IS NULL", "s = 'k0100'", "s IN ('k0001', 'k9999', 'zz')", "s IS NULL",
    "s = 'k0100' OR id = 7", "id = 5 AND v = 5",
]


def _typed_table(n=6000):
    ids = range(n)
    return pa.table({
        "i8": pa.array([None if i % 31 == 0 else (i % 250) - 125 for i in ids], pa.int8()),
        "u16": pa.array([None if i % 37 == 0 else i * 7 % 65536 for i in ids], pa.uint16()),
        "i32": pa.array([None if i % 41 == 0 else i * 1000 - 3000000 for i in ids], pa.int32()),
        "f32": pa.array([None if i % 43 == 0 else i / 3.0 for i in ids], pa.float32()),
        "d32": pa.array([None if i % 47 == 0 else i - 2000 for i in ids], pa.date32()),
        "ts": pa.array([None if i % 53 == 0 else i * 1_000_003 for i in ids], pa.timestamp("us", tz="UTC")),
        "ls": pa.array([None if i % 59 == 0 else f"{i}\u00e9" for i in ids], pa.large_string()),
        "bin": pa.array([None if i % 61 == 0 else bytes([i % 256, (i >> 8) % 256]) for i in ids], pa.binary()),
    })


@pytest.mark.parametrize("column", ["id", "v", "f", "s", "b"])
def test_zonemap_files_match_pylance(lance, tmp_path, column):
    paths = _both_build(lance, tmp_path, [column])
    theirs, theirs_meta = _zone_table(lance, paths["pylance"])
    ours, ours_meta = _zone_table(lance, paths["nanolance"])
    assert ours.schema == theirs.schema
    assert repr(ours.to_pylist()) == repr(theirs.to_pylist())  # NaN is no equal to itself
    assert ours_meta == theirs_meta == {"rows_per_zone": "8192", "null_bitmap": "1"}


def test_zonemap_nanolance_index_read_by_pylance(lance, tmp_path):
    path = _both_build(lance, tmp_path, ["id", "v", "f", "s", "b"])["nanolance"]
    for f in FILTERS:
        # pylance answers IS NOT NULL as NOT(IS NULL), which a zonemap (inexact) never answers under a NOT
        if f != "v IS NOT NULL":
            plan = lance.dataset(path).scanner(filter=f, columns=["id"]).explain_plan(verbose=False)
            assert "ScalarIndexQuery" in plan and "(ZoneMap)" in plan, f
        with_index = lance.dataset(path).to_table(filter=f, columns=["id"])
        without = lance.dataset(path).to_table(filter=f, columns=["id"], use_scalar_index=False)
        assert _ids(with_index) == _ids(without), f


def test_zonemap_pylance_index_read_by_nanolance(lance, tmp_path):
    path = _both_build(lance, tmp_path, ["id", "v", "f", "s", "b"])["pylance"]
    for f in FILTERS:
        plan = nl.dataset(path).scanner(filter=f, columns=["id"]).explain_plan()
        assert "ZoneMap" in plan, f
        assert _ids(nl.dataset(path).to_table(filter=f, columns=["id"])) == _ids(
            lance.dataset(path).to_table(filter=f, columns=["id"], use_scalar_index=False)), f


def test_zonemap_rows_per_zone_spans_and_nulls(lance, tmp_path):
    path = _fresh(tmp_path, "zones.lance")
    nl.write_dataset(_table(10000), path, max_rows_per_file=5000)
    nl.dataset(path).delete("id >= 1200 AND id < 1300")
    nl.dataset(path).create_scalar_index("id", "ZONEMAP", name="zm", rows_per_zone=1000)
    zones, meta = _zone_table(lance, path)
    assert meta["rows_per_zone"] == "1000"
    # Zones hold 1000 live rows each; a deleted row leaves a gap in the span (zone 1: offsets 1000-2099).
    rows = zones.to_pylist()
    assert [(z["fragment_id"], z["zone_start"], z["zone_length"]) for z in rows] == [
        (0, 0, 1000), (0, 1000, 1100), (0, 2100, 1000), (0, 3100, 1000), (0, 4100, 900),
        (1, 0, 1000), (1, 1000, 1000), (1, 2000, 1000), (1, 3000, 1000), (1, 4000, 1000)]
    assert (rows[1]["min"], rows[1]["max"]) == (1000, 2099)
    assert _ids(lance.dataset(path).to_table(filter="id BETWEEN 1100 AND 1400", columns=["id"])) == list(
        range(1100, 1200)) + list(range(1300, 1401))
    assert _ids(nl.dataset(path).to_table(filter="id BETWEEN 1100 AND 1400", columns=["id"])) == list(
        range(1100, 1200)) + list(range(1300, 1401))


def test_zonemap_nan_and_null_statistics_match_pylance(lance, tmp_path):
    table = pa.table({"x": pa.array([1.0, float("nan"), None, 3.0], pa.float64())})
    ours = _fresh(tmp_path, "nan_nano.lance")
    theirs = _fresh(tmp_path, "nan_lance.lance")
    nl.write_dataset(table, ours)
    lance.write_dataset(table, theirs)
    nl.dataset(ours).create_scalar_index("x", "ZONEMAP")
    lance.dataset(theirs).create_scalar_index("x", "ZONEMAP")
    a, _ = _zone_table(lance, ours)
    b, _ = _zone_table(lance, theirs)
    (za,), (zb,) = a.to_pylist(), b.to_pylist()
    assert (za["min"], za["null_count"], za["nan_count"]) == (zb["min"], zb["null_count"], zb["nan_count"]) == (1.0, 1, 1)
    assert str(za["max"]) == str(zb["max"]) == "nan"
    assert nl.dataset(ours).to_table(filter="x > 2.0", columns=["x"]).num_rows == 2  # 3.0 and NaN


def test_zonemap_maintenance_matches_pylance(lance, tmp_path):
    paths = _both_build(lance, tmp_path, ["id", "v"], rows=6000, fragments=2)
    for engine, path in paths.items():
        module = nl if engine == "nanolance" else lance
        module.write_dataset(_table(100, start=6000), path, mode="append")
    for engine in ("nanolance", "pylance"):
        ds = nl.dataset(paths[engine]) if engine == "nanolance" else None
        if ds is not None:
            ds.delete("id >= 5000 AND id < 5010")
            ds.update({"v": "v + 1000000"}, where="id >= 100 AND id < 110")
        else:
            lance.dataset(paths[engine]).delete("id >= 5000 AND id < 5010")
            lance.dataset(paths[engine]).update({"v": "v + 1000000"}, where="id >= 100 AND id < 110")

    def results(path):
        return {f: _ids(lance.dataset(path).to_table(filter=f, columns=["id"])) for f in FILTERS[:12]}

    scans = results(paths["nanolance"])
    assert scans == results(paths["pylance"])
    assert scans == {f: _ids(lance.dataset(paths["pylance"]).to_table(filter=f, columns=["id"],
                                                                      use_scalar_index=False))
                     for f in FILTERS[:12]}
    for engine in ("nanolance", "pylance"):
        module = nl if engine == "nanolance" else lance
        module.dataset(paths[engine]).optimize.optimize_indices()
        assert lance.dataset(paths[engine]).stats.index_stats("zm_id")["num_unindexed_rows"] == 0
    assert results(paths["nanolance"]) == scans
    assert results(paths["pylance"]) == scans


def test_zonemap_survives_compaction_with_stable_row_ids(lance, tmp_path):
    path = _fresh(tmp_path, "stable.lance")
    nl.write_dataset(_table(6000), path, enable_stable_row_ids=True, max_rows_per_file=2000)
    nl.dataset(path).create_scalar_index("id", "ZONEMAP", name="zm")
    nl.write_dataset(_table(500, start=6000), path, mode="append", enable_stable_row_ids=True)
    nl.dataset(path).delete("id >= 100 AND id < 200")
    nl.dataset(path).optimize.compact_files(target_rows_per_fragment=6000)
    nl.dataset(path).optimize.optimize_indices()
    for f in ("id = 150", "id = 6400", "id BETWEEN 90 AND 210"):
        expected = _ids(lance.dataset(path).to_table(filter=f, columns=["id"], use_scalar_index=False))
        assert _ids(nl.dataset(path).to_table(filter=f, columns=["id"])) == expected
        assert _ids(lance.dataset(path).to_table(filter=f, columns=["id"])) == expected


def test_zonemap_segments_committed_from_python(lance, tmp_path):
    path = _fresh(tmp_path, "segments.lance")
    nl.write_dataset(_table(6000), path, max_rows_per_file=2000)
    ds = nl.dataset(path)
    fragments = [f.fragment_id for f in lance.dataset(path).get_fragments()]
    segments = [ds.create_index_uncommitted("id", "ZONEMAP", name="zmseg", fragment_ids=[f]) for f in fragments]
    nl.dataset(path).commit_existing_index_segments("zmseg", "id", segments)
    indices = lance.dataset(path).list_indices()  # one entry per segment
    assert indices and {(i["name"], i["type"]) for i in indices} == {("zmseg", "ZoneMap")}
    plan = lance.dataset(path).scanner(filter="id = 5000", columns=["id"]).explain_plan(verbose=False)
    assert "ScalarIndexQuery" in plan and "@zmseg(ZoneMap)" in plan
    assert _ids(lance.dataset(path).to_table(filter="id BETWEEN 1990 AND 2010", columns=["id"])) == list(
        range(1990, 2011))


def test_zonemap_refuses_list_columns(tmp_path):
    path = _fresh(tmp_path, "lists.lance")
    nl.write_dataset(pa.table({"id": pa.array([1, 2], pa.int64()),
                               "tags": pa.array([["a"], ["b"]], pa.list_(pa.string()))}), path)
    with pytest.raises(Exception, match="a list column takes a LABEL_LIST index, not a ZONEMAP"):
        nl.dataset(path).create_scalar_index("tags", "ZONEMAP")


def _overlay(m, path, name, values, fields, offsets):
    """Commit `values` as a data overlay of the one fragment of the dataset at `path` (test_data_overlay's form)."""
    import os

    ds = m.dataset(path)
    with m.file.LanceFileWriter(os.path.join(path, "data", name)) as w:
        w.write_batch(values)
    base = ds.get_fragments()[0].metadata.files[0]
    df = m.fragment.DataFile(path=name, fields=fields, column_indices=list(range(len(fields))),
                             file_major_version=base.file_major_version, file_minor_version=base.file_minor_version,
                             file_size_bytes=os.path.getsize(os.path.join(path, "data", name)))
    op = m.LanceOperation.DataOverlay([m.LanceOperation.DataOverlayGroup(
        ds.get_fragments()[0].fragment_id, [m.LanceOperation.DataOverlayFile(df, offsets=offsets)])])
    m.LanceDataset.commit(path, op, read_version=ds.version)


def test_zonemap_under_a_data_overlay(lance, tmp_path, monkeypatch):
    # pylance 12 opens a dataset with overlays only with this set (as test_data_overlay.py does).
    monkeypatch.setenv("LANCE_ENABLE_UNSTABLE_DATA_OVERLAY_FILES", "1")
    table = pa.table({"id": pa.array(range(10), pa.int32()), "val": pa.array([i * 10 for i in range(10)], pa.int32())})
    expected = {"val = 999": [1], "val = 10": [], "val BETWEEN 0 AND 35": [0, 2, 3], "val > 85": [1, 9]}
    for m in (nl, lance):
        path = _fresh(tmp_path, f"overlay_{m.__name__.split('.')[0]}.lance")
        m.write_dataset(table, path)
        m.dataset(path).create_scalar_index("val", "ZONEMAP", name="zm")
        # the new values of ids 1 and 3 (the overlay names offsets 1 and 3 of the fragment)
        _overlay(m, path, "over.lance", pa.table({"val": pa.array([999, 1], pa.int32())}), [1], [1, 3])
        for f, ids in expected.items():
            assert _ids(m.dataset(path).to_table(filter=f, columns=["id"])) == ids, (m.__name__, f)
            assert _ids(m.dataset(path).to_table(filter=f, columns=["id"], use_scalar_index=False)) == ids, (
                m.__name__, f)


@pytest.mark.parametrize("column", ["id", "v", "f", "s"])
def test_bloomfilter_files_match_pylance(lance, tmp_path, column):
    paths = _both_build(lance, tmp_path, [column], kind="BLOOMFILTER")
    theirs, theirs_meta = _bloom_table(lance, paths["pylance"], f"bf_{column}")
    ours, ours_meta = _bloom_table(nl, paths["nanolance"], f"bf_{column}")
    assert ours.schema == theirs.schema
    assert ours.to_pylist() == theirs.to_pylist()  # the filters' bytes, bit for bit
    assert ours_meta == theirs_meta == {"bloomfilter_item": "8192", "bloomfilter_probability": "0.00057",
                                        "null_bitmap": "1"}


def test_bloomfilter_typed_columns_match_pylance(lance, tmp_path):
    table = _typed_table()
    names = table.column_names
    paths = {}
    for engine in ("pylance", "nanolance"):
        path = _fresh(tmp_path, f"typed_{engine}.lance")
        module = lance if engine == "pylance" else nl
        module.write_dataset(table.slice(0, 3000), path)
        module.write_dataset(table.slice(3000), path, mode="append")
        for column in names:
            module.dataset(path).create_scalar_index(column, "BLOOMFILTER", name=f"bf_{column}")
        paths[engine] = path
    for column in names:
        theirs, _ = _bloom_table(lance, paths["pylance"], f"bf_{column}")
        ours, _ = _bloom_table(nl, paths["nanolance"], f"bf_{column}")
        assert ours.to_pylist() == theirs.to_pylist(), column


def test_bloomfilter_nanolance_index_read_by_pylance(lance, tmp_path):
    path = _both_build(lance, tmp_path, ["id", "v", "f", "s"], kind="BLOOMFILTER")["nanolance"]
    for f in FILTERS_BLOOM:
        plan = lance.dataset(path).scanner(filter=f, columns=["id"]).explain_plan(verbose=False)
        assert "ScalarIndexQuery" in plan and "(BloomFilter)" in plan, f
        assert _ids(lance.dataset(path).to_table(filter=f, columns=["id"])) == _ids(
            lance.dataset(path).to_table(filter=f, columns=["id"], use_scalar_index=False)), f


def test_bloomfilter_pylance_index_read_by_nanolance(lance, tmp_path):
    path = _both_build(lance, tmp_path, ["id", "v", "f", "s"], kind="BLOOMFILTER")["pylance"]
    for f in FILTERS_BLOOM:
        plan = nl.dataset(path).scanner(filter=f, columns=["id"]).explain_plan()
        assert "BloomFilter" in plan, f
        assert _ids(nl.dataset(path).to_table(filter=f, columns=["id"])) == _ids(
            lance.dataset(path).to_table(filter=f, columns=["id"], use_scalar_index=False)), f


def test_bloomfilter_maintenance_matches_pylance(lance, tmp_path):
    paths = _both_build(lance, tmp_path, ["id", "s"], rows=6000, kind="BLOOMFILTER")
    for engine, path in paths.items():
        module = nl if engine == "nanolance" else lance
        module.write_dataset(_table(100, start=6000), path, mode="append")
        module.dataset(path).delete("id >= 5000 AND id < 5010")
    for engine, path in paths.items():
        module = nl if engine == "nanolance" else lance
        module.dataset(path).optimize.optimize_indices()
    for engine, path in paths.items():
        module = nl if engine == "nanolance" else lance
        module.dataset(path).optimize.compact_files()
    for path in paths.values():
        for f in FILTERS_BLOOM:
            assert _ids(nl.dataset(path).to_table(filter=f, columns=["id"])) == _ids(
                lance.dataset(path).to_table(filter=f, columns=["id"], use_scalar_index=False)), f


def test_bloomfilter_refuses_unsupported_types(lance, tmp_path):
    path = _fresh(tmp_path, "refused.lance")
    decimal = pa.array([1, 2], pa.decimal128(10, 2))
    nl.write_dataset(pa.table({"id": pa.array([1, 2], pa.int64()), "b": pa.array([True, False]),
                               "d": decimal, "l": pa.array([["a"], ["b"]], pa.list_(pa.string()))}), path)
    for column in ("b", "d", "l"):
        with pytest.raises(Exception):
            lance.dataset(path).create_scalar_index(column, "BLOOMFILTER")
        with pytest.raises(Exception, match="BLOOMFILTER"):
            nl.dataset(path).create_scalar_index(column, "BLOOMFILTER")


def test_bloomfilter_segments_committed_from_python(lance, tmp_path):
    path = _fresh(tmp_path, "bsegments.lance")
    nl.write_dataset(_table(6000), path, max_rows_per_file=2000)
    ds = nl.dataset(path)
    fragments = [f.fragment_id for f in lance.dataset(path).get_fragments()]
    segments = [ds.create_index_uncommitted("id", "BLOOMFILTER", name="bfseg", fragment_ids=[f]) for f in fragments]
    nl.dataset(path).commit_existing_index_segments("bfseg", "id", segments)
    indices = lance.dataset(path).list_indices()  # one entry per segment
    assert indices and {(i["name"], i["type"]) for i in indices} == {("bfseg", "BloomFilter")}
    plan = lance.dataset(path).scanner(filter="id = 5000", columns=["id"]).explain_plan(verbose=False)
    assert "ScalarIndexQuery" in plan and "@bfseg(BloomFilter)" in plan
    assert _ids(lance.dataset(path).to_table(filter="id = 5000", columns=["id"])) == [5000]


def test_bloomfilter_index_stats_match_pylance(lance, tmp_path):
    paths = _both_build(lance, tmp_path, ["id"], kind="BLOOMFILTER")
    theirs = lance.dataset(paths["pylance"]).stats.index_stats("bf_id")
    ours = nl.dataset(paths["nanolance"]).stats.index_stats("bf_id")
    assert ours["index_type"] == theirs["index_type"] == "BloomFilter"
    assert ours["indices"] == theirs["indices"] == [{"type": "BloomFilter", "num_blocks": 2,
                                                     "number_of_items": 8192, "probability": 0.00057}]


def test_bloomfilter_survives_compaction_with_stable_row_ids(lance, tmp_path):
    path = _fresh(tmp_path, "bstable.lance")
    nl.write_dataset(_table(6000), path, enable_stable_row_ids=True, max_rows_per_file=2000)
    nl.dataset(path).create_scalar_index("id", "BLOOMFILTER", name="bf")
    nl.write_dataset(_table(500, start=6000), path, mode="append", enable_stable_row_ids=True)
    nl.dataset(path).delete("id >= 100 AND id < 200")
    nl.dataset(path).optimize.compact_files(target_rows_per_fragment=6000)
    nl.dataset(path).optimize.optimize_indices()
    for f in ("id = 150", "id = 6400", "id IN (90, 210, 6499)"):
        expected = _ids(lance.dataset(path).to_table(filter=f, columns=["id"], use_scalar_index=False))
        assert _ids(nl.dataset(path).to_table(filter=f, columns=["id"])) == expected
        assert _ids(lance.dataset(path).to_table(filter=f, columns=["id"])) == expected


@pytest.mark.parametrize("items,probability", [(1000, 0.01), (100000, 0.0001), (3, 0.00057), (10**6, 0.5)])
def test_bloomfilter_parameters_match_pylance(lance, tmp_path, items, probability):
    from lance.indices import IndexConfig

    paths = {}
    for engine in ("pylance", "nanolance"):
        path = _fresh(tmp_path, f"params_{engine}.lance")
        module = lance if engine == "pylance" else nl
        module.write_dataset(_table(20000), path, max_rows_per_file=20000)
        if engine == "pylance":
            lance.dataset(path).create_scalar_index(
                "id", IndexConfig(index_type="BLOOMFILTER", parameters={"number_of_items": items,
                                                                       "probability": probability}), name="bf")
        else:
            nl.dataset(path).create_scalar_index("id", "BLOOMFILTER", name="bf", number_of_items=items,
                                                 probability=probability)
        paths[engine] = path
    theirs, theirs_meta = _bloom_table(lance, paths["pylance"], "bf")
    ours, ours_meta = _bloom_table(nl, paths["nanolance"], "bf")
    assert ours.to_pylist() == theirs.to_pylist()
    assert ours_meta == theirs_meta
    assert ours_meta["bloomfilter_item"] == str(items)
