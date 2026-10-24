"""MemWAL metadata (M1): ``initialize_mem_wal``, the ``__lance_mem_wal`` index, sharding specs and the bucket hash.

The bar is pylance's: a MemWAL index either engine initializes is the one the other reads back with the
same ``mem_wal_index_details()``; the index survives the operations a dataset goes through; and
``evaluate_sharding_spec`` routes every value to the bucket pylance routes it to. The writers, memtables
and LSM scanners are not implemented; their names raise ``NotSupportedError``.
"""

from __future__ import annotations

import datetime
import decimal
import os
import shutil

import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance._errors import NotSupportedError
from nanolance.lance import mem_wal as nmw
from tests.support import require_pylance

PK_META = {"lance-schema:unenforced-primary-key": "true"}
PK_SCHEMA = pa.schema([pa.field("id", pa.int64(), nullable=False, metadata=PK_META), pa.field("name", pa.utf8())])
APPEND_SCHEMA = pa.schema([pa.field("id", pa.int64(), nullable=False), pa.field("name", pa.utf8())])
MEM_WAL = "__lance_mem_wal"


def _rows(ids, prefix, schema=PK_SCHEMA):
    return pa.table({"id": pa.array(ids, pa.int64()), "name": pa.array([f"{prefix}_{i}" for i in ids])},
                    schema=schema)


def _write(engine, path, table, schema=PK_SCHEMA):
    return engine.write_dataset(table, path, schema=schema)


def _mem_wal_entries(ds):
    return [(i["name"], i["type"], i["version"], sorted(i["fields"]), sorted(i["fragment_ids"]))
            for i in ds.list_indices() if i["name"] == MEM_WAL]


def _mem_wal_description(ds):
    described = [d for d in ds.describe_indices() if d.name == MEM_WAL]
    assert len(described) <= 1
    if not described:
        return None
    d = described[0]
    segments = [(s.dataset_version_at_last_update, s.index_version, sorted(s.fragment_ids), s.size_bytes,
                 s.base_id, s.covering_fields) for s in d.segments]
    return (d.type_url, d.index_type, d.fields, d.field_names, d.num_rows_indexed, d.total_size_bytes, d.details,
            segments)


# Each initialize form: the keyword arguments, and the table it runs on.
INIT_FORMS = {
    "manual": ({}, PK_SCHEMA),
    "unsharded": ({"unsharded": True}, PK_SCHEMA),
    "bucket": ({"bucket_column": "name", "num_buckets": 8}, PK_SCHEMA),
    "bucket_without_primary_key": ({"bucket_column": "id", "num_buckets": 8}, APPEND_SCHEMA),
    "identity": ({"identity_column": "name"}, PK_SCHEMA),
    "identity_int": ({"identity_column": "id"}, PK_SCHEMA),
    "writer_defaults": ({"durable_write": False, "max_memtable_size": 8 * 1024 * 1024,
                         "max_wal_flush_interval_ms": 250}, PK_SCHEMA),
    "writer_defaults_zero_flush": ({"max_wal_flush_interval_ms": 0}, PK_SCHEMA),
    "hnsw_only": ({"hnsw_params": {"emb": {"num_edges": 8}}}, PK_SCHEMA),
    "maintained_btree": ({"maintained_indexes": ["id_btree"]}, PK_SCHEMA),
    "maintained_and_bucket": ({"maintained_indexes": ["id_btree"], "bucket_column": "id", "num_buckets": 4,
                               "stats_log_interval_ms": 1000}, PK_SCHEMA),
}


def _prepare(engine_dir, schema, maintained: bool):
    """A dataset with the indexes the forms name, written by pylance (the base both engines start from)."""
    lance = require_pylance()
    path = os.path.join(engine_dir, "base")
    ds = _write(lance, path, _rows([1, 2, 3], "base", schema), schema)
    if maintained:
        ds.create_scalar_index("id", "BTREE", name="id_btree")
    return path


@pytest.mark.parametrize("form", list(INIT_FORMS))
def test_initialize_mem_wal_forms_agree(tmp_path, form):
    lance = require_pylance()
    kwargs, schema = INIT_FORMS[form]
    base = _prepare(str(tmp_path), schema, maintained="maintained_indexes" in kwargs)
    p_path, n_path = str(tmp_path / "pylance"), str(tmp_path / "nanolance")
    shutil.copytree(base, p_path)
    shutil.copytree(base, n_path)

    p_ds = lance.dataset(p_path)
    p_ds.initialize_mem_wal(**kwargs)
    n_ds = nl.dataset(n_path)
    n_ds.initialize_mem_wal(**kwargs)

    expected = p_ds.mem_wal_index_details()
    assert expected is not None
    assert n_ds.mem_wal_index_details() == expected
    assert _mem_wal_entries(n_ds) == _mem_wal_entries(p_ds)
    assert _mem_wal_description(n_ds) == _mem_wal_description(p_ds)

    # Each engine reads the other's index back.
    assert nl.dataset(p_path).mem_wal_index_details() == expected
    assert lance.dataset(n_path).mem_wal_index_details() == expected


def test_nanolance_index_is_listed_and_described_like_pylance(tmp_path):
    lance = require_pylance()
    base = _prepare(str(tmp_path), PK_SCHEMA, maintained=False)
    n_path = str(tmp_path / "n")
    shutil.copytree(base, n_path)
    nl.dataset(n_path).initialize_mem_wal(bucket_column="name", num_buckets=8)
    p_path = str(tmp_path / "p")
    shutil.copytree(base, p_path)
    lance.dataset(p_path).initialize_mem_wal(bucket_column="name", num_buckets=8)

    n_ds, p_ds = nl.dataset(n_path), lance.dataset(p_path)
    assert n_ds.has_index is True
    assert _mem_wal_entries(n_ds) == _mem_wal_entries(p_ds)
    assert _mem_wal_description(n_ds) == _mem_wal_description(p_ds)


def test_initialize_mem_wal_twice_is_refused(tmp_path):
    lance = require_pylance()
    base = _prepare(str(tmp_path), PK_SCHEMA, maintained=False)
    for engine in (nl, lance):
        path = str(tmp_path / engine.__name__.replace(".", "_"))
        shutil.copytree(base, path)
        ds = engine.dataset(path)
        ds.initialize_mem_wal()
        with pytest.raises(OSError, match="already initialized"):
            ds.initialize_mem_wal()


@pytest.mark.parametrize("kwargs, error", [
    ({"unsharded": True, "bucket_column": "id", "num_buckets": 8}, ValueError),
    ({"bucket_column": "id"}, ValueError),
    ({"num_buckets": 8}, ValueError),
    ({"identity_column": "id", "unsharded": True}, ValueError),
    ({"bucket_column": "id", "num_buckets": 0}, OSError),
    ({"bucket_column": "id", "num_buckets": 1025}, OSError),
    ({"bucket_column": "missing", "num_buckets": 4}, OSError),
    ({"identity_column": "missing"}, OSError),
    ({"identity_column": "name", "unsharded": False}, None),
    ({"maintained_indexes": ["missing"]}, OSError),
    ({"hnsw_params": {"x": {"bogus": 1}}}, ValueError),
])
def test_initialize_mem_wal_rejects_like_pylance(tmp_path, kwargs, error):
    lance = require_pylance()
    base = _prepare(str(tmp_path), PK_SCHEMA, maintained=False)
    outcomes = []
    for engine in (lance, nl):
        path = str(tmp_path / engine.__name__.replace(".", "_"))
        shutil.copytree(base, path)
        ds = engine.dataset(path)
        try:
            ds.initialize_mem_wal(**kwargs)
            outcomes.append(None)
        except Exception as exc:  # noqa: BLE001 -- compared to pylance's outcome below, never swallowed
            outcomes.append(type(exc))
    assert outcomes[1] is outcomes[0]
    assert outcomes[0] is error


def test_initialize_mem_wal_rejects_maintained_kind_pylance_refuses(tmp_path):
    lance = require_pylance()
    base = _prepare(str(tmp_path), PK_SCHEMA, maintained=False)
    for engine in (lance, nl):
        path = str(tmp_path / engine.__name__.replace(".", "_"))
        shutil.copytree(base, path)
        ds = engine.dataset(path)
        ds.create_scalar_index("name", "BITMAP", name="name_bm")
        with pytest.raises(OSError, match="cannot maintain"):
            ds.initialize_mem_wal(maintained_indexes=["name_bm"])


@pytest.mark.parametrize("operation", ["append", "delete", "update", "merge_insert", "compact", "optimize_indices",
                                       "cleanup"])
def test_mem_wal_index_survives_operations_on_a_pylance_dataset(tmp_path, operation):
    lance = require_pylance()
    base = _prepare(str(tmp_path), PK_SCHEMA, maintained=True)
    lance.dataset(base).initialize_mem_wal(bucket_column="name", num_buckets=8,
                                           maintained_indexes=["id_btree"], durable_write=False)
    p_path, n_path = str(tmp_path / "pylance"), str(tmp_path / "nanolance")
    shutil.copytree(base, p_path)
    shutil.copytree(base, n_path)
    expected_entries = _mem_wal_entries(lance.dataset(p_path))
    expected_details = lance.dataset(p_path).mem_wal_index_details()
    assert expected_entries

    def run(engine, path):
        if operation == "append":
            engine.write_dataset(_rows([4, 5], "more"), path, schema=PK_SCHEMA, mode="append")
        elif operation == "delete":
            engine.dataset(path).delete("id = 2")
        elif operation == "update":
            engine.dataset(path).update({"name": "'changed'"}, where="id = 3")
        elif operation == "merge_insert":
            (engine.dataset(path).merge_insert("id").when_matched_update_all().when_not_matched_insert_all()
             .execute(_rows([3, 100], "merged")))
        elif operation == "compact":
            engine.dataset(path).optimize.compact_files(target_rows_per_fragment=1000)
        elif operation == "optimize_indices":
            engine.dataset(path).optimize.optimize_indices()
        elif operation == "cleanup":
            engine.dataset(path).cleanup_old_versions(retain_versions=1, delete_unverified=True)

    run(lance, p_path)
    run(nl, n_path)

    n_ds = nl.dataset(n_path)
    assert _mem_wal_entries(n_ds) == expected_entries
    assert n_ds.mem_wal_index_details() == expected_details
    assert _mem_wal_entries(lance.dataset(p_path)) == expected_entries
    assert n_ds.count_rows() == lance.dataset(p_path).count_rows()


def test_pylance_lists_the_mem_wal_index_as_nanolance_does(tmp_path):
    lance = require_pylance()
    base = _prepare(str(tmp_path), PK_SCHEMA, maintained=False)
    path = str(tmp_path / "p")
    shutil.copytree(base, path)
    lance.dataset(path).initialize_mem_wal()
    names = [i["name"] for i in nl.dataset(path).list_indices()]
    assert names == [i["name"] for i in lance.dataset(path).list_indices()] == [MEM_WAL]


# ---------------------------------------------------------------------------
# evaluate_sharding_spec: the bucket hash, against pylance's
# ---------------------------------------------------------------------------

TD = datetime.timedelta
D = datetime.date(2001, 2, 3)
EVAL_COLUMNS = {
    "int8": pa.array([1, -1, None, 127, -128], pa.int8()),
    "int16": pa.array([1, -300, None, 32767], pa.int16()),
    "int32": pa.array([1, 2, None, 3], pa.int32()),
    "int64": pa.array([1, -1, None, 2**40, -(2**40), 2**63 - 1, -(2**63)], pa.int64()),
    "uint8": pa.array([0, 1, None, 255], pa.uint8()),
    "uint16": pa.array([0, 65535, None], pa.uint16()),
    "uint32": pa.array([0, 2**31, None, 2**32 - 1], pa.uint32()),
    "uint64": pa.array([0, 2**63, None, 2**64 - 1], pa.uint64()),
    "bool": pa.array([True, False, None], pa.bool_()),
    "float32": pa.array([0.0, -0.0, 1.5, None, -2.25, 3.0e38], pa.float32()),
    "float32_nan": pa.array([float("nan"), 1.0, None], pa.float32()),
    "float64": pa.array([0.0, -0.0, 1.5, None, -2.25, 1e300], pa.float64()),
    "float64_nan": pa.array([float("nan"), 2.0], pa.float64()),
    "date32": pa.array([D, datetime.date(1970, 1, 1), None], pa.date32()),
    "timestamp_s": pa.array([0, -5, None, 1700000000], pa.timestamp("s")),
    "timestamp_ms": pa.array([0, 1700000000123, None], pa.timestamp("ms")),
    "timestamp_us_tz": pa.array([0, -1, None], pa.timestamp("us", tz="UTC")),
    "timestamp_ns": pa.array([0, 123456789, None], pa.timestamp("ns")),
    "time32_s": pa.array([0, 86399, None], pa.time32("s")),
    "time32_ms": pa.array([1, 86399999, None], pa.time32("ms")),
    "time64_us": pa.array([0, 86399999999, None], pa.time64("us")),
    "time64_ns": pa.array([0, 1, None], pa.time64("ns")),
    "utf8": pa.array(["", "a", "ab", "abc", "abcd", "abcde", "abcdef", "abcdefg", "abcdefgh", "abcdefghi", None,
                      "héllo", "日本語", "🙂 emoji", "\x00\x7f\x80\xff", "k3"], pa.utf8()),
    "large_utf8": pa.array(["x", "longer value with spaces", None, "é"], pa.large_utf8()),
}


@pytest.mark.parametrize("name", list(EVAL_COLUMNS))
@pytest.mark.parametrize("num_buckets", [1, 8, 1000])
def test_evaluate_sharding_spec_bucket_matches_pylance(name, num_buckets):
    lance = require_pylance()
    column = EVAL_COLUMNS[name]
    batch = pa.record_batch([column], names=["key"])
    spec = {"spec_id": 1, "fields": [{"field_id": "bucket", "source_ids": [0], "transform": "bucket",
                                      "expression": None, "result_type": "int32",
                                      "parameters": {"num_buckets": str(num_buckets)}}]}
    expected = lance.mem_wal.evaluate_sharding_spec(batch, spec, lance.schema.LanceSchema.from_pyarrow(batch.schema))
    actual = nmw.evaluate_sharding_spec(batch, spec, nl.schema.LanceSchema.from_pyarrow(batch.schema))
    assert actual.column_names == expected.column_names == ["bucket"]
    assert actual.column(0).type == expected.column(0).type == pa.int32()
    assert actual.column(0).to_pylist() == expected.column(0).to_pylist()


@pytest.mark.parametrize("name", ["int32", "int64", "utf8", "large_utf8", "uint8", "bool", "date32", "utf8"])
def test_evaluate_sharding_spec_identity_and_unsharded_match_pylance(name):
    lance = require_pylance()
    column = EVAL_COLUMNS[name]
    batch = pa.record_batch([column], names=["key"])
    schema_p = lance.schema.LanceSchema.from_pyarrow(batch.schema)
    schema_n = nl.schema.LanceSchema.from_pyarrow(batch.schema)
    identity = {"spec_id": 1, "fields": [{"field_id": "k", "source_ids": [0], "transform": "identity",
                                          "expression": None, "result_type": "int32", "parameters": {}}]}
    unsharded = {"spec_id": 2, "fields": [{"field_id": "u", "source_ids": [], "transform": "unsharded",
                                           "expression": None, "result_type": "int32", "parameters": {}}]}
    for spec in (identity, unsharded):
        expected = lance.mem_wal.evaluate_sharding_spec(batch, spec, schema_p)
        actual = nmw.evaluate_sharding_spec(batch, spec, schema_n)
        assert actual.column_names == expected.column_names
        assert actual.column(0).to_pylist() == expected.column(0).to_pylist()


def test_evaluate_sharding_spec_nested_column_and_parameter_matches_pylance():
    lance = require_pylance()
    struct = pa.array([{"x": 3}, {"x": -4}, None], pa.struct([pa.field("x", pa.int64())]))
    batch = pa.record_batch([pa.array([1, 2, 3], pa.int32()), struct], names=["a", "s"])
    spec_by_source = {"spec_id": 1, "fields": [{"field_id": "b", "source_ids": [2], "transform": "bucket",
                                                "expression": None, "result_type": "int32",
                                                "parameters": {"num_buckets": "16"}}]}
    spec_by_column = {"spec_id": 1, "fields": [{"field_id": "b", "source_ids": [], "transform": "bucket",
                                                "expression": None, "result_type": "int32",
                                                "parameters": {"num_buckets": "16", "column": "a"}}]}
    # A nested source (field id 2 is s.x) is not a column of the batch: Lance looks columns up by name at
    # the top level only, and refuses it. A `column` parameter naming a top-level column is used as is.
    with pytest.raises(ValueError, match="Sharding source column 's.x' not found in batch"):
        lance.mem_wal.evaluate_sharding_spec(batch, spec_by_source, lance.schema.LanceSchema.from_pyarrow(batch.schema))
    with pytest.raises(ValueError, match="Sharding source column 's.x' not found in batch"):
        nmw.evaluate_sharding_spec(batch, spec_by_source, nl.schema.LanceSchema.from_pyarrow(batch.schema))
    expected = lance.mem_wal.evaluate_sharding_spec(batch, spec_by_column,
                                                    lance.schema.LanceSchema.from_pyarrow(batch.schema))
    actual = nmw.evaluate_sharding_spec(batch, spec_by_column, nl.schema.LanceSchema.from_pyarrow(batch.schema))
    assert actual.column(0).to_pylist() == expected.column(0).to_pylist()


def test_initialize_bucket_spec_evaluates_like_pylance(tmp_path):
    lance = require_pylance()
    base = _prepare(str(tmp_path), PK_SCHEMA, maintained=False)
    path = str(tmp_path / "n")
    shutil.copytree(base, path)
    nl.dataset(path).initialize_mem_wal(bucket_column="name", num_buckets=8)
    spec = nl.dataset(path).mem_wal_index_details()["sharding_specs"][0]
    batch = _rows([1, 2, 3], "base").to_batches()[0]
    expected = lance.mem_wal.evaluate_sharding_spec(batch, spec, lance.schema.LanceSchema.from_pyarrow(batch.schema))
    actual = nmw.evaluate_sharding_spec(batch, spec, nl.schema.LanceSchema.from_pyarrow(batch.schema))
    assert actual.column(0).to_pylist() == expected.column(0).to_pylist()


def test_evaluate_sharding_spec_rejects_unsupported_transform():
    batch = pa.record_batch([pa.array([1])], names=["a"])
    spec = {"spec_id": 1, "fields": [{"field_id": "x", "source_ids": [0], "transform": "year",
                                      "expression": None, "result_type": "int32", "parameters": {}}]}
    with pytest.raises(ValueError, match="Unsupported MemWAL sharding transform"):
        nmw.evaluate_sharding_spec(batch, spec, nl.schema.LanceSchema.from_pyarrow(batch.schema))


@pytest.mark.parametrize("name", ["M2 classes"])
def test_mem_wal_writers_and_scanners_are_refused_by_name(name):
    with pytest.raises(NotSupportedError, match="ShardWriter"):
        nmw.ShardWriter(None)
    with pytest.raises(NotSupportedError, match="CompactedSsTable"):
        nmw.CompactedSsTable("00000000-0000-0000-0000-000000000000", 1)
    with pytest.raises(NotSupportedError, match="LsmScanner"):
        nmw.LsmScanner.from_snapshots(None)
    with pytest.raises(NotSupportedError, match="ShardSnapshot"):
        nmw.ShardSnapshot("00000000-0000-0000-0000-000000000000")


def test_mem_wal_index_details_none_before_init(tmp_path):
    lance = require_pylance()
    path = str(tmp_path / "ds")
    lance.write_dataset(_rows([1, 2, 3], "base"), path, schema=PK_SCHEMA)
    assert nl.dataset(path).mem_wal_index_details() is None
    assert nl.dataset(path).list_indices() == []
    assert not nl.dataset(path).has_index


def test_mem_wal_writer_is_refused_by_name(tmp_path):
    path = str(tmp_path / "ds")
    nl.write_dataset(_rows([1], "base"), path, schema=PK_SCHEMA)
    with pytest.raises(NotSupportedError, match="mem_wal_writer"):
        nl.dataset(path).mem_wal_writer("00000000-0000-0000-0000-000000000000")
