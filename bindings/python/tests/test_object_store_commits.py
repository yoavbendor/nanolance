# SPDX-License-Identifier: Apache-2.0
"""Writes to s3:// datasets and their commit protocol (docs/OBJECT_STORE_WRITES.md, phase W1).

Skipped unless NANOLANCE_S3_TEST_ENDPOINT names an S3-compatible endpoint that honours conditional
writes (moto locally and in CI). Every dataset goes under a fresh prefix of NANOLANCE_S3_TEST_BUCKET.
The fault cases run nanolance in a child process with NANOS3READER_FAULTS set (nanos3reader reads the
rules once per process).
"""

import json
import os
import subprocess
import sys
import textwrap
import uuid
from datetime import timedelta

import numpy as np
import pyarrow as pa
import pytest

from tests.support import require_pylance

import nanolance.lance as nl
from nanolance import _nanolance
from nanolance.lance.fragment import DataFile, write_fragments
from nanolance.lance.indices import IndexConfig, IndicesBuilder
from nanolance.lance.schema import LanceSchema
from nanolance.lance.udf import BatchUDF

ENDPOINT = os.environ.get("NANOLANCE_S3_TEST_ENDPOINT", "")
BUCKET = os.environ.get("NANOLANCE_S3_TEST_BUCKET", "nl-test")

pytestmark = pytest.mark.skipif(not ENDPOINT, reason="NANOLANCE_S3_TEST_ENDPOINT is not set (no S3 endpoint)")


def options() -> dict:
    return {"aws_access_key_id": os.environ.get("AWS_ACCESS_KEY_ID", "testing"),
            "aws_secret_access_key": os.environ.get("AWS_SECRET_ACCESS_KEY", "testing"),
            "aws_region": os.environ.get("AWS_REGION", "us-east-1"), "aws_endpoint": ENDPOINT,
            "allow_http": "true"}


def fresh_uri() -> str:
    return f"s3://{BUCKET}/nanolance-commits-{uuid.uuid4().hex}/ds.lance"


def batch(start: int, rows: int = 100) -> pa.Table:
    return pa.table({"id": pa.array(range(start, start + rows), pa.int64()),
                     "name": pa.array([f"row {i}" for i in range(start, start + rows)])})


# A child process: `appends` appends of 100 rows each to `uri`, then a JSON line of how many succeeded and
# the errors of the others.
CHILD = textwrap.dedent("""
    import json, os, sys
    if os.environ.get("NANOLANCE_TEST_CHILD_PRELUDE"):  # e.g. to import a worktree's nanolance
        exec(open(os.environ["NANOLANCE_TEST_CHILD_PRELUDE"]).read())
    import pyarrow as pa
    import nanolance.lance as nl
    uri, appends, first = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    options = json.loads(sys.argv[5])
    ok, errors = 0, []
    for i in range(appends):
        start = first + i * 100
        t = pa.table({"id": pa.array(range(start, start + 100), pa.int64()),
                      "name": pa.array([f"row {j}" for j in range(start, start + 100)])})
        try:
            nl.write_dataset(t, uri, mode="append", storage_options=options)
            ok += 1
        except OSError as e:
            errors.append(str(e))
    print(json.dumps({"ok": ok, "errors": errors}))
""")


def start_appender(uri: str, appends: int, first: int, faults: str = "") -> subprocess.Popen:
    env = dict(os.environ, NANOS3READER_FAULTS=faults)
    return subprocess.Popen([sys.executable, "-c", CHILD, uri, str(appends), str(first), "[]",
                             json.dumps(options())], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)


def result(proc: subprocess.Popen) -> dict:
    out, err = proc.communicate(timeout=600)
    assert proc.returncode == 0, err
    return json.loads(out.strip().splitlines()[-1])


def test_create_append_overwrite_read_by_pylance():
    lance = require_pylance()
    uri = fresh_uri()
    assert nl.write_dataset(batch(0), uri, storage_options=options()).version == 1
    assert nl.write_dataset(batch(100), uri, mode="append", storage_options=options()).count_rows() == 200
    with pytest.raises(OSError, match="already exists"):
        nl.write_dataset(batch(0), uri, mode="create", storage_options=options())
    assert nl.write_dataset(batch(0, 10), uri, mode="overwrite", storage_options=options()).version == 3
    theirs = lance.dataset(uri, storage_options=options())
    assert theirs.version == 3
    assert [lance.dataset(uri, version=v, storage_options=options()).count_rows() for v in (1, 2, 3)] == [100, 200, 10]
    assert [theirs.read_transaction(v).operation.__class__.__name__ for v in (1, 2, 3)] == \
        ["Overwrite", "Append", "Overwrite"]
    assert theirs.to_table().to_pydict() == batch(0, 10).to_pydict()


def test_a_data_file_larger_than_the_memory_buffer_uploads_whole():
    # Objects over 8 MiB spill to a temporary file before the upload; that path once uploaded 0 bytes.
    lance = require_pylance()
    uri = fresh_uri()
    rows = 400_000
    table = pa.table({"id": pa.array(range(rows), pa.int64()),
                      "payload": pa.array([i.to_bytes(8, "little") * 4 for i in range(rows)], pa.binary())})
    nl.write_dataset(table, uri, storage_options=options())
    for ds in (nl.dataset(uri, storage_options=options()), lance.dataset(uri, storage_options=options())):
        assert ds.to_table().sort_by("id").equals(table)


def test_packed_and_dedicated_blobs_read_back_from_s3():
    # Blobs above the inline limit live in sidecar files, which were once opened with std::ifstream only.
    uri = fresh_uri()
    blobs = [bytes([1]) * 100, bytes([2]) * 70_000, bytes([3]) * 5_000_000]  # inline, packed, dedicated
    nl.write_dataset(pa.table({"id": pa.array([0, 1, 2], pa.int64()), "blob": nl.blob_array(blobs)}), uri,
                     storage_options=options())
    files = nl.dataset(uri, storage_options=options()).take_blobs("blob", indices=[0, 1, 2])
    assert [f.read() for f in files] == blobs


def test_pylance_and_nanolance_append_to_one_dataset():
    lance = require_pylance()
    uri = fresh_uri()
    lance.write_dataset(batch(0), uri, storage_options=options())
    nl.write_dataset(batch(100), uri, mode="append", storage_options=options())
    lance.write_dataset(batch(200), uri, mode="append", storage_options=options())
    ours = nl.dataset(uri, storage_options=options())
    assert ours.version == 3
    assert sorted(ours.to_table(columns=["id"])["id"].to_pylist()) == list(range(300))
    assert lance.dataset(uri, storage_options=options()).count_rows() == 300


def test_lost_reply_of_the_manifest_put_commits_once():
    # The first manifest PUT is applied, its reply dropped; the retried create-only PUT then sees the
    # manifest it wrote itself (HTTP 412). That is this commit, not a conflict: one version, no copy.
    uri = fresh_uri()
    nl.write_dataset(batch(0), uri, storage_options=options())
    outcome = result(start_appender(uri, 1, 100, faults="lost:PUT:.manifest:1"))
    assert outcome == {"ok": 1, "errors": []}
    ds = nl.dataset(uri, storage_options=options())
    assert ds.version == 2
    assert sorted(ds.to_table(columns=["id"])["id"].to_pylist()) == list(range(200))


def test_racing_writers_lose_and_double_nothing():
    uri = fresh_uri()
    nl.write_dataset(batch(0), uri, storage_options=options())
    procs = [start_appender(uri, 6, 100), start_appender(uri, 6, 10_000)]
    outcomes = [result(p) for p in procs]
    assert all(o == {"ok": 6, "errors": []} for o in outcomes), outcomes
    ds = nl.dataset(uri, storage_options=options())
    assert ds.version == 13
    expected = list(range(100)) + list(range(100, 700)) + list(range(10_000, 10_600))
    assert sorted(ds.to_table(columns=["id"])["id"].to_pylist()) == sorted(expected)


def test_a_store_that_ignores_if_none_match_is_refused():
    # Every PUT under _versions/ without If-None-Match: a store on which racing writers would overwrite
    # each other's versions. nanolance probes it before its first commit and refuses to commit there.
    uri = fresh_uri()
    nl.write_dataset(batch(0), uri, storage_options=options())
    procs = [start_appender(uri, 3, 100, faults="nocond:PUT:_versions/:*"),
             start_appender(uri, 3, 10_000, faults="nocond:PUT:_versions/:*")]
    outcomes = [result(p) for p in procs]
    for o in outcomes:
        assert o["ok"] == 0, o
        assert len(o["errors"]) == 3 and all("does not honour conditional writes" in e for e in o["errors"]), o
    ds = nl.dataset(uri, storage_options=options())
    assert ds.version == 1 and ds.count_rows() == 100


# ── W2a: mutations on s3:// (docs/OBJECT_STORE_W2.md) ──────────────────────────────────────────────
# Each operation runs on an s3:// dataset and on a local copy of the same data; pylance reads both and must
# see the same versions, rows, transaction kinds, config and tags.

def _merge_source() -> pa.Table:
    return pa.table({"id": pa.array([1, 2, 500], pa.int64()), "name": pa.array(["m1", "m2", "m500"])})


def _commit_fragment_delete(ds, uri: str, storage) -> None:
    meta = ds.get_fragments()[0].delete("id = 1")
    nl.LanceDataset.commit(uri, nl.LanceOperation.Delete([meta], [], "id = 1"), read_version=ds.version,
                           storage_options=storage)


W2A_OPS = {
    "delete": lambda ds, uri, storage: ds.delete("id < 10"),
    "update": lambda ds, uri, storage: ds.update({"name": "'x'"}, where="id = 1"),
    "update_all": lambda ds, uri, storage: ds.update({"name": "'y'"}),
    "merge_insert": lambda ds, uri, storage: ds.merge_insert("id").when_matched_update_all()
                                                 .when_not_matched_insert_all().execute(_merge_source()),
    "restore": lambda ds, uri, storage: (ds.delete("id < 5"), ds.restore()),
    "update_config": lambda ds, uri, storage: ds.update_config({"k": "v"}),
    "delete_config_keys": lambda ds, uri, storage: (ds.update_config({"k": "v", "j": "w"}),
                                                    ds.delete_config_keys(["k"])),
    "update_metadata": lambda ds, uri, storage: ds.update_metadata({"k": "v"}),
    "update_schema_metadata": lambda ds, uri, storage: ds.update_schema_metadata({"k": "v"}),
    "update_field_metadata": lambda ds, uri, storage: ds.update_field_metadata({"id": {"unit": "ms"}}),
    "fragment_delete": _commit_fragment_delete,
    "tag_create": lambda ds, uri, storage: ds.tags.create("t1", 1),
    "tag_update": lambda ds, uri, storage: (ds.delete("id < 5"), ds.tags.create("t1", 1), ds.tags.update("t1", 2)),
    "tag_delete": lambda ds, uri, storage: (ds.tags.create("t1", 1), ds.tags.delete("t1")),
    "tag_replace_metadata": lambda ds, uri, storage: (ds.tags.create("t1", 1),
                                                      ds.tags.replace_metadata("t1", {"a": "b"})),
}


def _snapshot(uri: str, storage) -> dict:
    lance = require_pylance()
    ds = lance.dataset(uri, storage_options=storage)
    versions = [v["version"] for v in ds.versions()]
    return {
        "versions": versions,
        "transactions": [type(ds.read_transaction(v).operation).__name__ for v in versions],
        "tables": [lance.dataset(uri, version=v, storage_options=storage).to_table().sort_by("id").to_pydict()
                   for v in versions],
        "config": dict(ds.config()),
        "metadata": dict(ds.metadata),
        "schema_metadata": ds.schema.metadata,
        "schemas": [str(lance.dataset(uri, version=v, storage_options=storage).schema) for v in versions],
        "tags": sorted((name, info["version"]) for name, info in ds.tags.list().items()),
    }


@pytest.mark.parametrize("name", list(W2A_OPS))
def test_mutation_on_s3_matches_the_local_copy(name, tmp_path):
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    for uri, storage in ((s3_uri, options()), (local, None)):
        nl.write_dataset(batch(0), uri, storage_options=storage)
        W2A_OPS[name](nl.dataset(uri, storage_options=storage), uri, storage)
    assert _snapshot(s3_uri, options()) == _snapshot(local, None)


def test_pylance_delete_then_nanolance_update_on_s3():
    lance = require_pylance()
    uri = fresh_uri()
    nl.write_dataset(batch(0), uri, storage_options=options())
    lance.dataset(uri, storage_options=options()).delete("id < 10")
    assert nl.dataset(uri, storage_options=options()).update({"name": "'x'"}, where="id = 50") == \
        {"num_rows_updated": 1}
    theirs = lance.dataset(uri, storage_options=options())
    assert theirs.version == 3 and theirs.count_rows() == 90
    assert [theirs.read_transaction(v).operation.__class__.__name__ for v in (1, 2, 3)] == \
        ["Overwrite", "Delete", "Update"]
    assert theirs.to_table(filter="id = 50").column("name").to_pylist() == ["x"]


# A child process: deletes of each predicate in turn, then a JSON line of the rows deleted and the errors.
DELETER = textwrap.dedent("""
    import json, sys
    import nanolance.lance as nl
    uri, predicates, options = sys.argv[1], json.loads(sys.argv[2]), json.loads(sys.argv[3])
    deleted, errors = 0, []
    for predicate in predicates:
        try:
            deleted += nl.dataset(uri, storage_options=options).delete(predicate)["num_deleted_rows"]
        except OSError as e:
            errors.append(str(e))
    print(json.dumps({"deleted": deleted, "errors": errors}))
""")


def test_racing_deletes_lose_and_double_nothing():
    uri = fresh_uri()
    nl.write_dataset(batch(0), uri, storage_options=options())
    first = [f"id >= {a} and id < {a + 10}" for a in (0, 10, 20, 30)]
    second = [f"id >= {a} and id < {a + 10}" for a in (60, 70, 80, 90)]
    procs = [subprocess.Popen([sys.executable, "-c", DELETER, uri, json.dumps(p), json.dumps(options())],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
             for p in (first, second)]
    outcomes = [result(p) for p in procs]
    assert outcomes == [{"deleted": 40, "errors": []}, {"deleted": 40, "errors": []}], outcomes
    ds = nl.dataset(uri, storage_options=options())
    assert ds.version == 9
    assert sorted(ds.to_table(columns=["id"])["id"].to_pylist()) == list(range(40, 60))


def test_tag_conflicts_and_missing_tags_as_locally():
    uri = fresh_uri()
    nl.write_dataset(batch(0), uri, storage_options=options())
    ds = nl.dataset(uri, storage_options=options())
    ds.tags.create("t1", 1)
    with pytest.raises(ValueError, match="Ref conflict error: tag t1 already exists"):
        ds.tags.create("t1", 1)
    ds.tags.delete("t1")
    with pytest.raises(ValueError, match="Ref not found error: tag t1 does not exist"):
        ds.tags.delete("t1")
    assert nl.dataset(uri, storage_options=options()).tags.list() == {}


# ── W2b: schema changes and compaction on s3:// (docs/OBJECT_STORE_W2.md) ──────────────────────────────

def _wide(start: int, rows: int = 100) -> pa.Table:
    return batch(start, rows).append_column("score", pa.array(range(start, start + rows), pa.int64()))


def _two_fragments(uri: str, storage) -> None:
    nl.write_dataset(_wide(0), uri, storage_options=storage)
    nl.write_dataset(_wide(100), uri, mode="append", storage_options=storage)


def _new_column_data(ds) -> pa.RecordBatchReader:
    values = pa.array(range(ds.count_rows()), pa.int64())
    return pa.RecordBatchReader.from_batches(pa.schema([pa.field("extra", pa.int64())]),
                                             [pa.record_batch([values], names=["extra"])])


def _compact_in_steps(ds) -> None:
    from nanolance.lance.lance import Compaction

    rewrites = [task.execute(ds) for task in Compaction.plan(ds).tasks]
    Compaction.commit(ds, rewrites)


W2B_OPS = {
    "add_columns_sql": lambda ds: ds.add_columns({"extra": "id + 1"}),
    "add_columns_nulls": lambda ds: ds.add_columns(pa.field("extra", pa.int64())),
    "add_columns_data": lambda ds: ds.add_columns(_new_column_data(ds)),
    "drop_columns": lambda ds: ds.drop_columns(["score"]),
    "alter_columns": lambda ds: ds.alter_columns({"path": "score", "name": "score2"}),
    "alter_columns_nullable": lambda ds: ds.alter_columns({"path": "score", "nullable": True}),
    "compact_files": lambda ds: ds.optimize.compact_files(),
    "compact_files_after_delete": lambda ds: (ds.delete("id < 10"), ds.optimize.compact_files()),
    "compact_in_steps": lambda ds: (ds.delete("id < 10"), _compact_in_steps(ds)),
}


@pytest.mark.parametrize("name", list(W2B_OPS))
def test_schema_change_or_compaction_on_s3_matches_the_local_copy(name, tmp_path):
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    for uri, storage in ((s3_uri, options()), (local, None)):
        _two_fragments(uri, storage)
        W2B_OPS[name](nl.dataset(uri, storage_options=storage))
    assert _snapshot(s3_uri, options()) == _snapshot(local, None)


def test_pylance_add_column_then_nanolance_drop_and_compact_on_s3():
    lance = require_pylance()
    uri = fresh_uri()
    _two_fragments(uri, options())
    lance.dataset(uri, storage_options=options()).add_columns({"extra": "id + 1"})
    ds = nl.dataset(uri, storage_options=options())
    ds.drop_columns(["score"])
    ds.optimize.compact_files()
    theirs = lance.dataset(uri, storage_options=options())
    # Compaction first commits a version that reserves the new fragments' ids (a BaseOperation), then the rewrite.
    assert theirs.version == 6
    assert [theirs.read_transaction(v).operation.__class__.__name__ for v in range(1, 7)] == \
        ["Overwrite", "Append", "Merge", "Project", "BaseOperation", "Rewrite"]
    assert theirs.schema.names == ["id", "name", "extra"]
    table = theirs.to_table().sort_by("id")
    assert table.column("extra").to_pylist() == [i + 1 for i in range(200)]


# A child process: adds one new column (SQL) per name given, then a JSON line of the names it added and the
# errors of the others.
ADDER = textwrap.dedent("""
    import json, sys
    import nanolance.lance as nl
    uri, names, options = sys.argv[1], json.loads(sys.argv[2]), json.loads(sys.argv[3])
    added, errors = [], []
    for name in names:
        try:
            nl.dataset(uri, storage_options=options).add_columns({name: "id + 1"})
            added.append(name)
        except OSError as e:
            errors.append(str(e))
    print(json.dumps({"added": added, "errors": errors}))
""")


def test_racing_add_columns_land_or_conflict_and_lose_nothing():
    lance = require_pylance()
    uri = fresh_uri()
    _two_fragments(uri, options())
    names = [["a1", "a2"], ["b1", "b2"]]
    procs = [subprocess.Popen([sys.executable, "-c", ADDER, uri, json.dumps(n), json.dumps(options())],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for n in names]
    outcomes = [result(p) for p in procs]
    added = sorted(n for o in outcomes for n in o["added"])
    assert all("conflict" in e.lower() for o in outcomes for e in o["errors"]), outcomes
    theirs = lance.dataset(uri, storage_options=options())
    assert theirs.version == 2 + len(added)
    assert sorted(theirs.schema.names) == sorted(["id", "name", "score"] + added)


# A child process: `appends` appends of 100 rows, then a JSON line of the ones that succeeded and the errors.
COMPACTOR = textwrap.dedent("""
    import json, sys
    import nanolance.lance as nl
    uri, options = sys.argv[1], json.loads(sys.argv[2])
    try:
        ds = nl.dataset(uri, storage_options=options)
        metrics = ds.optimize.compact_files()
        print(json.dumps({"fragments_removed": metrics.fragments_removed, "errors": []}))
    except OSError as e:
        print(json.dumps({"fragments_removed": 0, "errors": [str(e)]}))
""")


def test_compaction_racing_an_append_loses_no_rows():
    lance = require_pylance()
    uri = fresh_uri()
    _two_fragments(uri, options())
    appender = start_appender(uri, 3, 200)
    compactor = subprocess.Popen([sys.executable, "-c", COMPACTOR, uri, json.dumps(options())],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    appended = result(appender)
    compacted = result(compactor)
    assert appended["errors"] == [] and compacted["errors"] == [], (appended, compacted)
    theirs = lance.dataset(uri, storage_options=options())
    ids = sorted(theirs.to_table(columns=["id"])["id"].to_pylist())
    assert ids == list(range(200 + 100 * appended["ok"]))



# ── W3a: index builds and maintenance on s3:// (docs/OBJECT_STORE_W3.md) ─────────────────────────────
# Each index is built on an s3:// dataset and on a local copy; nanolance and pylance both read the S3 one,
# and the answers must equal the local copy's. Trained vector models differ from one build to the next
# (k-means and PQ start from random seeds), so for the approximate kinds the check is the S3 index against
# itself in both engines, and against the local copy by overlap.

INDEX_ROWS = 600
INDEX_FRAGMENT_ROWS = 200


def _index_table() -> pa.Table:
    rng = np.random.default_rng(7)
    n = INDEX_ROWS
    return pa.table({
        "id": pa.array(range(n), pa.int64()),
        "name": pa.array([f"row {i}" for i in range(n)]),
        "text": pa.array([f"alpha beta gamma{i % 13}" for i in range(n)]),
        "cat": pa.array([f"c{i % 5}" for i in range(n)]),
        "tags": pa.array([[f"t{i % 7}", "x"] for i in range(n)], pa.list_(pa.string())),
        "js": pa.array([f'{{"a": {i % 9}}}' for i in range(n)], pa.json_(pa.utf8())),
        "vec": pa.FixedSizeListArray.from_arrays(pa.array(rng.random(n * 8).astype("float32")), 8),
        "bits": pa.FixedSizeListArray.from_arrays(pa.array(rng.integers(0, 256, n * 8, dtype=np.uint8)), 8),
    })


def _indexed_copy(uri: str, storage, build) -> None:
    nl.write_dataset(_index_table(), uri, storage_options=storage, max_rows_per_file=INDEX_FRAGMENT_ROWS)
    build(nl.dataset(uri, storage_options=storage))


def _nanolance_json_config():
    return IndexConfig(index_type="json", parameters={"target_index_type": "btree", "path": "a"})


def _pylance_json_config():
    from lance.indices import IndexConfig as PylanceIndexConfig

    return PylanceIndexConfig(index_type="json", parameters={"target_index_type": "btree", "path": "a"})


# Scalar kinds: the builder (nanolance's, or pylance's for the reverse check) and the queries.
SCALAR_BUILDS = {
    "BTREE": lambda ds, json_config: ds.create_scalar_index("id", "BTREE", name="idx"),
    "BITMAP": lambda ds, json_config: ds.create_scalar_index("cat", "BITMAP", name="idx"),
    "LABEL_LIST": lambda ds, json_config: ds.create_scalar_index("tags", "LABEL_LIST", name="idx"),
    "INVERTED": lambda ds, json_config: ds.create_scalar_index("text", "INVERTED", name="idx"),
    "NGRAM": lambda ds, json_config: ds.create_scalar_index("name", "NGRAM", name="idx"),
    "ZONEMAP": lambda ds, json_config: ds.create_scalar_index("id", "ZONEMAP", name="idx"),
    "BLOOMFILTER": lambda ds, json_config: ds.create_scalar_index("id", "BLOOMFILTER", name="idx"),
    "JSON": lambda ds, json_config: ds.create_scalar_index("js", json_config(), name="idx"),
}
SCALAR_QUERIES = {
    "BTREE": [{"filter": "id = 77"}, {"filter": "id BETWEEN 100 AND 110"}],
    "BITMAP": [{"filter": "cat = 'c1'"}, {"filter": "cat IN ('c0', 'c4')"}],
    "LABEL_LIST": [{"filter": "array_has_any(tags, ['t3'])"}, {"filter": "array_has_all(tags, ['t0', 'x'])"}],
    "INVERTED": [{"full_text_query": "gamma3"}, {"full_text_query": "beta alpha"}],
    "NGRAM": [{"filter": "name LIKE '%row 12%'"}],
    "ZONEMAP": [{"filter": "id = 77"}, {"filter": "id > 590"}],
    "BLOOMFILTER": [{"filter": "id = 77"}, {"filter": "id IN (5, 6)"}],
    "JSON": [{"filter": "json_get_int(js, 'a') = 3"}],
}
# Vector kinds, with what must equal the local copy's answer: "rows" (every partition is searched, so the
# answer does not depend on the training), "distances" (ties at the k-th place may swap rows), or None.
VECTOR_BUILDS = {
    "IVF_FLAT": (dict(index_type="IVF_FLAT", num_partitions=4), "vec", "rows"),
    "IVF_SQ": (dict(index_type="IVF_SQ", num_partitions=4), "vec", "rows"),
    "IVF_PQ": (dict(index_type="IVF_PQ", num_partitions=4, num_sub_vectors=4), "vec", None),
    "IVF_RQ": (dict(index_type="IVF_RQ", num_partitions=4), "vec", None),
    "IVF_HNSW_FLAT": (dict(index_type="IVF_HNSW_FLAT", num_partitions=2), "vec", None),
    "IVF_HNSW_SQ": (dict(index_type="IVF_HNSW_SQ", num_partitions=2), "vec", None),
    "IVF_HNSW_PQ": (dict(index_type="IVF_HNSW_PQ", num_partitions=2, num_sub_vectors=4), "vec", None),
    "HAMMING": (dict(index_type="IVF_FLAT", num_partitions=4, metric="hamming"), "bits", "distances"),
}
VECTOR_QUERY = np.random.default_rng(11).random(8).astype("float32")
NEAREST_K = 7


def _near(column: str):
    query = VECTOR_QUERY if column == "vec" else np.arange(8, dtype=np.uint8) * 31
    return {"nearest": {"column": column, "q": query, "k": NEAREST_K}}


def _answers(ds, queries) -> list:
    """Each query's rows: (id, score or distance) sorted, floats rounded to 4 places."""
    out = []
    for kw in queries:
        extra = ["_score"] if "full_text_query" in kw else ["_distance"] if "nearest" in kw else []
        table = ds.to_table(columns=["id", *extra], **kw).to_pydict()
        rows = zip(*(table[c] for c in ["id", *extra]))
        out.append(sorted(tuple(round(v, 4) if isinstance(v, float) else v for v in row) for row in rows))
    return out


def _index_names(ds) -> list:
    return sorted((i["name"], tuple(i["fields"])) for i in ds.list_indices())


def _uses_index(lance, uri: str, flt: str) -> bool:
    return "ScalarIndexQuery" in lance.dataset(uri, storage_options=options()).scanner(filter=flt).explain_plan()


@pytest.mark.parametrize("name", list(SCALAR_BUILDS))
def test_scalar_index_on_s3_answers_as_the_local_copy(name, tmp_path):
    lance = require_pylance()
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    _indexed_copy(s3_uri, options(), lambda ds: SCALAR_BUILDS[name](ds, _nanolance_json_config))
    _indexed_copy(local, None, lambda ds: SCALAR_BUILDS[name](ds, _nanolance_json_config))
    queries = SCALAR_QUERIES[name]
    ours = _answers(nl.dataset(s3_uri, storage_options=options()), queries)
    assert ours == _answers(nl.dataset(local), queries)
    assert ours == _answers(lance.dataset(s3_uri, storage_options=options()), queries)  # pylance reads it
    assert _index_names(nl.dataset(s3_uri, storage_options=options())) == _index_names(nl.dataset(local))
    for kw in queries:
        if "filter" in kw and name != "NGRAM":
            assert _uses_index(lance, s3_uri, kw["filter"]), kw


@pytest.mark.parametrize("name", list(SCALAR_BUILDS))
def test_scalar_index_built_by_pylance_on_s3_is_read_by_nanolance(name):
    lance = require_pylance()
    uri = fresh_uri()
    _indexed_copy(uri, options(), lambda ds: None)
    SCALAR_BUILDS[name](lance.dataset(uri, storage_options=options()), _pylance_json_config)
    queries = SCALAR_QUERIES[name]
    assert _answers(nl.dataset(uri, storage_options=options()), queries) == \
        _answers(lance.dataset(uri, storage_options=options()), queries)


@pytest.mark.parametrize("name", list(VECTOR_BUILDS))
def test_vector_index_on_s3_answers_as_the_local_copy(name, tmp_path):
    lance = require_pylance()
    params, column, exact = VECTOR_BUILDS[name]
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    _indexed_copy(s3_uri, options(), lambda ds: ds.create_index(column, name="idx", **params))
    _indexed_copy(local, None, lambda ds: ds.create_index(column, name="idx", **params))
    queries = [_near(column)]
    ours = _answers(nl.dataset(s3_uri, storage_options=options()), queries)
    theirs = _answers(lance.dataset(s3_uri, storage_options=options()), queries)
    assert ours == theirs  # the same S3 index, read by both engines
    assert _index_names(nl.dataset(s3_uri, storage_options=options())) == _index_names(nl.dataset(local))
    local_answer = _answers(nl.dataset(local), queries)
    if exact == "rows":
        assert ours == local_answer
    elif exact == "distances":
        assert [sorted(row[1] for row in q) for q in ours] == [sorted(row[1] for row in q) for q in local_answer]
    else:  # trained afresh on each build: the nearest seven must mostly agree
        assert len({row[0] for row in ours[0]} & {row[0] for row in local_answer[0]}) >= NEAREST_K - 2


@pytest.mark.parametrize("name", list(VECTOR_BUILDS))
def test_vector_index_built_by_pylance_on_s3_is_read_by_nanolance(name):
    lance = require_pylance()
    params, column, _ = VECTOR_BUILDS[name]
    uri = fresh_uri()
    _indexed_copy(uri, options(), lambda ds: None)
    lance.dataset(uri, storage_options=options()).create_index(column, name="idx", **params)
    queries = [_near(column)]
    assert _answers(nl.dataset(uri, storage_options=options()), queries) == \
        _answers(lance.dataset(uri, storage_options=options()), queries)


def _snapshot_with_indices(uri: str, storage) -> dict:
    lance = require_pylance()
    snap = _snapshot(uri, storage)
    snap["indices"] = [(v, _index_names(lance.dataset(uri, version=v, storage_options=storage)))
                       for v in snap["versions"]]
    return snap


def test_drop_index_on_s3_matches_the_local_copy(tmp_path):
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    for uri, storage in ((s3_uri, options()), (local, None)):
        def build(ds):
            ds.create_scalar_index("id", "BTREE", name="id_idx")
            ds.create_scalar_index("text", "INVERTED", name="fts")
            ds.drop_index("id_idx")
        _indexed_copy(uri, storage, build)
    assert _snapshot_with_indices(s3_uri, options()) == _snapshot_with_indices(local, None)
    assert _index_names(nl.dataset(s3_uri, storage_options=options())) == [("fts", ("text",))]


def test_optimize_indices_on_s3_matches_the_local_copy(tmp_path):
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    for uri, storage in ((s3_uri, options()), (local, None)):
        def build(ds):
            ds.create_scalar_index("id", "BTREE", name="id_idx")
            ds.create_scalar_index("text", "INVERTED", name="fts")
            ds.create_index("vec", index_type="IVF_FLAT", num_partitions=4, name="vec_idx")
        _indexed_copy(uri, storage, build)
        nl.write_dataset(_index_table().slice(0, 50), uri, mode="append", storage_options=storage)
        nl.dataset(uri, storage_options=storage).optimize.optimize_indices()
    assert _snapshot_with_indices(s3_uri, options()) == _snapshot_with_indices(local, None)
    queries = [{"filter": "id = 3"}, {"full_text_query": "gamma3"}, _near("vec")]
    lance = require_pylance()
    assert _answers(lance.dataset(s3_uri, storage_options=options()), queries) == \
        _answers(nl.dataset(local), queries)


DISTRIBUTED_SCALAR = {
    "BTREE": ("id", "BTREE", ["id < 100", "id >= 500 AND id < 520", "id IN (3, 333, 500)"]),
    "BITMAP": ("cat", "BITMAP", ["cat = 'c3'", "cat IN ('c1', 'c4')"]),
    "LABEL_LIST": ("tags", "LABEL_LIST", ["array_has_any(tags, ['t2'])", "array_has_all(tags, ['t0', 'x'])"]),
    "INVERTED": ("text", "INVERTED", None),
}


def _fragment_groups(ds) -> list:
    ids = [f.fragment_id for f in ds.get_fragments()]
    return [ids[:2], ids[2:]]


@pytest.mark.parametrize("kind", list(DISTRIBUTED_SCALAR))
def test_scalar_segments_built_on_s3_merged_and_committed(kind, tmp_path):
    lance = require_pylance()
    column, index_type, filters = DISTRIBUTED_SCALAR[kind]
    queries = [{"full_text_query": "gamma3"}] if filters is None else [{"filter": f} for f in filters]
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    nl.write_dataset(_index_table(), s3_uri, storage_options=options(), max_rows_per_file=INDEX_FRAGMENT_ROWS)
    ours = nl.dataset(s3_uri, storage_options=options())
    segments = [ours.create_index_uncommitted(column, index_type, name="idx", fragment_ids=g)
                for g in _fragment_groups(ours)]
    assert ours.list_indices() == []  # nothing committed yet
    merged = ours.merge_existing_index_segments(segments)
    ours.commit_existing_index_segments("idx", column, [merged])
    _indexed_copy(local, None, lambda ds: ds.create_scalar_index(column, index_type, name="idx"))
    answers = _answers(lance.dataset(s3_uri, storage_options=options()), queries)
    assert answers == _answers(nl.dataset(s3_uri, storage_options=options()), queries)
    assert answers == _answers(nl.dataset(local), queries)


def test_vector_segments_with_a_shared_model_built_on_s3(tmp_path):
    lance = require_pylance()
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    nl.write_dataset(_index_table(), s3_uri, storage_options=options(), max_rows_per_file=INDEX_FRAGMENT_ROWS)
    ours = nl.dataset(s3_uri, storage_options=options())
    centroids = IndicesBuilder(ours, "vec").train_ivf(4, sample_rate=8, max_iters=10).centroids
    segments = [ours.create_index_uncommitted("vec", "IVF_FLAT", name="vi", num_partitions=4, fragment_ids=g,
                                              ivf_centroids=centroids) for g in _fragment_groups(ours)]
    ours.commit_existing_index_segments("vi", "vec", [ours.merge_existing_index_segments(segments)])
    _indexed_copy(local, None, lambda ds: ds.create_index("vec", name="vi", index_type="IVF_FLAT", num_partitions=4))
    queries = [_near("vec")]
    answer = _answers(lance.dataset(s3_uri, storage_options=options()), queries)
    assert answer == _answers(nl.dataset(s3_uri, storage_options=options()), queries)
    assert answer == _answers(nl.dataset(local), queries)


def test_legacy_inverted_partial_build_stages_its_parts_on_s3(tmp_path):
    """create_scalar_index(fragment_ids, index_uuid) per fragment, then merge_index_metadata: the staged
    parts are written to and read from the object store, and removed once the segment is built. The
    LanceDataset.commit of that segment is W3b."""
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    outcome = {}
    for uri, storage in ((s3_uri, options()), (local, None)):
        nl.write_dataset(_index_table(), uri, storage_options=storage, max_rows_per_file=INDEX_FRAGMENT_ROWS)
        ds = nl.dataset(uri, storage_options=storage)
        index_id = str(uuid.uuid4())
        for fragment in ds.get_fragments():
            ds.create_scalar_index("text", "INVERTED", name="fts", replace=False, index_uuid=index_id,
                                   fragment_ids=[fragment.fragment_id], remove_stop_words=False)
        events = []
        ds.merge_index_metadata(index_id, index_type="INVERTED", progress_callback=events.append)
        outcome[uri] = [e.stage for e in events if e.event == "complete"]
        assert _nanolance._index_stage_read(uri, index_id) == []  # the parts are gone
    assert outcome[s3_uri] == outcome[local] == [
        "read_partition_metadata", "remap_partition_files", "write_merged_metadata"]


def _index_builder_child(uri: str, options_json: str) -> str:
    return textwrap.dedent(f"""
        import json
        import nanolance.lance as nl
        ds = nl.dataset({uri!r}, storage_options=json.loads({options_json!r}))
        ds.create_scalar_index("id", "BTREE", name="id_idx")
        print("built")
    """)


def test_index_build_racing_an_append_is_rebased_and_loses_no_rows():
    lance = require_pylance()
    uri = fresh_uri()
    nl.write_dataset(batch(0, INDEX_ROWS), uri, storage_options=options(), max_rows_per_file=INDEX_FRAGMENT_ROWS)
    builder = subprocess.Popen([sys.executable, "-c", _index_builder_child(uri, json.dumps(options()))],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    appender = start_appender(uri, 3, INDEX_ROWS)
    appended = result(appender)
    built = builder.communicate()
    assert appended["errors"] == [] and built[0].strip() == "built", (appended, built)
    theirs = lance.dataset(uri, storage_options=options())
    ids = sorted(theirs.to_table(columns=["id"])["id"].to_pylist())
    assert ids == list(range(INDEX_ROWS + 100 * appended["ok"]))
    assert [i["name"] for i in theirs.list_indices()] == ["id_idx"]
    assert theirs.to_table(filter="id = 77", columns=["id"])["id"].to_pylist() == [77]


def _objects_under(uri: str, prefix: str) -> list:
    """The object keys under `prefix` of the s3:// `uri`'s bucket, listed by pyarrow's S3 client."""
    from pyarrow import fs as pafs

    bucket, _, key = uri[len("s3://"):].partition("/")
    s3 = pafs.S3FileSystem(access_key=options()["aws_access_key_id"], secret_key=options()["aws_secret_access_key"],
                           region=options()["aws_region"], endpoint_override=ENDPOINT, scheme="http")
    selector = pafs.FileSelector(f"{bucket}/{key.rstrip('/')}/{prefix}", recursive=True, allow_not_found=True)
    return [info.path for info in s3.get_file_info(selector) if info.type == pafs.FileType.File]


def test_index_build_whose_manifest_is_refused_leaves_no_index_objects():
    # The build writes its index objects, then the manifest PUT fails on every attempt: the build fails
    # and removes the objects it wrote (remove_index_dir), so nothing under _indices/ is left behind.
    lance = require_pylance()
    uri = fresh_uri()
    nl.write_dataset(batch(0), uri, storage_options=options())
    child = _index_builder_child(uri, json.dumps(options()))
    proc = subprocess.run([sys.executable, "-c", child], capture_output=True, text=True, check=False,
                          env=dict(os.environ, NANOS3READER_FAULTS="fail:PUT:_versions/:*"))
    assert proc.returncode != 0 and "built" not in proc.stdout, proc
    theirs = lance.dataset(uri, storage_options=options())
    assert theirs.version == 1 and theirs.list_indices() == []
    assert _objects_under(uri, "_indices/") == []


# ── W3b: fragment-level writes and hand-built commits (docs/tasks/OBJECT_STORE_W3.md) ────────────────
# Each operation writes its fragments with nanolance on s3:// and on a local copy of the same dataset, then
# commits them; pylance reads both and must see the same versions, rows, schemas and transaction kinds.

def _new_fragment(uri: str, storage, start: int = 200, rows: int = 100):
    return nl.LanceFragment.create(uri, _wide(start, rows), storage_options=storage)


def _commit_append(uri: str, storage, fragments) -> None:
    ds = nl.dataset(uri, storage_options=storage)
    nl.LanceDataset.commit(uri, nl.LanceOperation.Append(fragments), read_version=ds.version, storage_options=storage)


def _commit_transaction(uri: str, storage, txn) -> None:
    nl.LanceDataset.commit(uri, txn, storage_options=storage)


def _reader_for(rows: int) -> pa.RecordBatchReader:
    values = pa.array(range(rows), pa.int64())
    return pa.RecordBatchReader.from_batches(pa.schema([pa.field("extra", pa.int64())]),
                                             [pa.record_batch([values], names=["extra"])])


def _extra_udf() -> BatchUDF:
    return BatchUDF(lambda b: pa.record_batch([b.column(0)], names=["extra"]))


def _merge_every_fragment(uri: str, storage, column_for) -> None:
    ds = nl.dataset(uri, storage_options=storage)
    fragments, schema = [], None
    for fragment in ds.get_fragments():
        meta, schema = column_for(fragment)
        fragments.append(meta)
    nl.LanceDataset.commit(uri, nl.LanceOperation.Merge(fragments, schema), read_version=ds.version,
                           storage_options=storage)


def _join_table() -> pa.Table:
    return pa.table({"id": pa.array([1, 2, 500], pa.int64()), "tag": pa.array(["a", "b", "c"])})


def _restore_by_hand(uri: str, storage) -> None:
    nl.dataset(uri, storage_options=storage).delete("id < 50")
    nl.LanceDataset.commit(uri, nl.LanceOperation.Restore(1), storage_options=storage)


def _update_by_hand(uri: str, storage) -> None:
    ds = nl.dataset(uri, storage_options=storage)
    removed = ds.get_fragments()[0].fragment_id
    nl.LanceDataset.commit(uri, nl.LanceOperation.Update(removed_fragment_ids=[removed],
                                                         new_fragments=[_new_fragment(uri, storage, 0)]),
                           read_version=ds.version, storage_options=storage)


def _rewrite_by_hand(uri: str, storage) -> None:
    ds = nl.dataset(uri, storage_options=storage)
    old = [f.metadata for f in ds.get_fragments()]
    merged = _new_fragment(uri, storage, 0, rows=200)
    nl.LanceDataset.commit(uri, nl.LanceOperation.Rewrite([nl.LanceOperation.RewriteGroup(old, [merged])], []),
                           read_version=ds.version, storage_options=storage)


def _config_by_hand(uri: str, storage) -> None:
    ds = nl.dataset(uri, storage_options=storage)
    update = nl.LanceOperation.UpdateMap({"k": "v"})
    nl.LanceDataset.commit(uri, nl.LanceOperation.UpdateConfig(config_updates=update), read_version=ds.version,
                           storage_options=storage)


def _project_without_score(uri: str, storage) -> None:
    """Lance's Project: the dataset's fields but ``score``, as the field messages the manifest holds."""
    ds = nl.dataset(uri, storage_options=storage)
    messages, metadata = _nanolance._ds_field_messages(ds.uri, ds.version)
    infos = _nanolance._ds_fields(ds.uri, ds.version)
    kept = [m for m, info in zip(messages, infos) if info["name"] != "score"]
    nl.LanceDataset.commit(uri, nl.LanceOperation.Project(LanceSchema._from_messages(kept, metadata)),
                           read_version=ds.version, storage_options=storage)


def _replace_score_by_hand(uri: str, storage) -> None:
    """Lance's DataReplacement: a file holding the new ``score`` values replaces the column of fragment 0."""
    ds = nl.dataset(uri, storage_options=storage)
    values = pa.table({"score": pa.array(range(1000, 1100), pa.int64())})
    new_file = DataFile.create(ds, str(nl.LanceFragment.create(uri, values, storage_options=storage).files[0].path))
    fragment = ds.get_fragments()[0].fragment_id
    nl.LanceDataset.commit(uri, nl.LanceOperation.DataReplacement(
        [nl.LanceOperation.DataReplacementGroup(fragment, new_file)]), read_version=ds.version,
        storage_options=storage)


def _batch_of_appends(uri: str, storage) -> None:
    ds = nl.dataset(uri, storage_options=storage)
    txns = [nl.Transaction(ds.version, nl.LanceOperation.Append([_new_fragment(uri, storage, start)]))
            for start in (300, 400)]
    nl.LanceDataset.commit_batch(uri, txns, storage_options=storage)


W3B_OPS = {
    "fragment_create_append": lambda uri, st: _commit_append(uri, st, [_new_fragment(uri, st)]),
    "fragment_create_from_file": lambda uri, st: _commit_append(uri, st, [nl.LanceFragment.create_from_file(
        str(_new_fragment(uri, st).files[0].path), nl.dataset(uri, storage_options=st), fragment_id=2)]),
    "write_fragments_append": lambda uri, st: _commit_transaction(uri, st, write_fragments(
        _wide(200), uri, mode="append", return_transaction=True, storage_options=st)),
    "write_fragments_overwrite": lambda uri, st: _commit_transaction(uri, st, write_fragments(
        _wide(200), uri, mode="overwrite", return_transaction=True, storage_options=st)),
    "write_fragments_split_in_files": lambda uri, st: _commit_transaction(uri, st, write_fragments(
        _wide(300), uri, mode="append", max_rows_per_file=100, return_transaction=True, storage_options=st)),
    "merge_columns_sql": lambda uri, st: _merge_every_fragment(
        uri, st, lambda f: f.merge_columns({"extra": "score + 1"})),
    "merge_columns_data": lambda uri, st: _merge_every_fragment(
        uri, st, lambda f: f.merge_columns(_reader_for(f.count_rows()))),
    "merge_columns_udf": lambda uri, st: _merge_every_fragment(
        uri, st, lambda f: f.merge_columns(_extra_udf(), columns=["score"])),
    "add_columns_from_a_function": lambda uri, st: nl.dataset(uri, storage_options=st).add_columns(_extra_udf()),
    "merge_join": lambda uri, st: nl.dataset(uri, storage_options=st).merge(_join_table(), "id"),
    "commit_overwrite": lambda uri, st: nl.LanceDataset.commit(
        uri, nl.LanceOperation.Overwrite(nl.dataset(uri, storage_options=st).lance_schema,
                                         [_new_fragment(uri, st, 300)]), storage_options=st),
    "commit_restore": _restore_by_hand,
    "commit_update": _update_by_hand,
    "commit_rewrite": _rewrite_by_hand,
    "commit_update_config": _config_by_hand,
    "commit_batch_of_appends": _batch_of_appends,
    "commit_project": _project_without_score,
    "commit_data_replacement": _replace_score_by_hand,
    "merge_columns_sql_after_delete": lambda uri, st: (nl.dataset(uri, storage_options=st).delete("id < 30"),
                                                       _merge_every_fragment(
                                                           uri, st, lambda f: f.merge_columns({"extra": "id * 2"}))),
    "add_columns_udf_after_delete": lambda uri, st: (nl.dataset(uri, storage_options=st).delete("id < 30"),
                                                     nl.dataset(uri, storage_options=st).add_columns(_extra_udf())),
}


@pytest.mark.parametrize("name", list(W3B_OPS))
def test_fragment_write_or_commit_on_s3_matches_the_local_copy(name, tmp_path):
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    for uri, storage in ((s3_uri, options()), (local, None)):
        _two_fragments(uri, storage)
        W3B_OPS[name](uri, storage)
    assert _snapshot(s3_uri, options()) == _snapshot(local, None)


def test_fragment_write_on_a_pylance_dataset_on_s3_matches_the_local_copy(tmp_path):
    lance = require_pylance()
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    for uri, storage in ((s3_uri, options()), (local, None)):
        lance.write_dataset(_wide(0), uri, storage_options=storage)
        _merge_every_fragment(uri, storage, lambda f: f.merge_columns({"extra": "score + 1"}))
    assert _snapshot(s3_uri, options()) == _snapshot(local, None)


# A child process: `commits` hand-built appends of 100 rows each, every one checked against the dataset as the
# commit finds it (a conflict is retried by the commit itself), then a JSON line of the rows it committed.
APPENDER = textwrap.dedent("""
    import json, sys
    import pyarrow as pa
    import nanolance.lance as nl
    uri, first, commits, options = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), json.loads(sys.argv[4])
    for i in range(commits):
        start = first + i * 100
        t = pa.table({"id": pa.array(range(start, start + 100), pa.int64()),
                      "name": pa.array([f"row {j}" for j in range(start, start + 100)]),
                      "score": pa.array(range(start, start + 100), pa.int64())})
        fragment = nl.LanceFragment.create(uri, t, storage_options=options)
        ds = nl.dataset(uri, storage_options=options)
        nl.LanceDataset.commit(uri, nl.LanceOperation.Append([fragment]), read_version=ds.version,
                               storage_options=options)
    print(json.dumps({"committed": commits}))
""")


def test_racing_hand_built_appends_lose_and_double_nothing():
    uri = fresh_uri()
    _two_fragments(uri, options())
    procs = [subprocess.Popen([sys.executable, "-c", APPENDER, uri, str(first), "3", json.dumps(options())],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
             for first in (1000, 2000)]
    outcomes = [result(p) for p in procs]
    assert outcomes == [{"committed": 3}, {"committed": 3}], outcomes
    ds = nl.dataset(uri, storage_options=options())
    assert ds.version == 2 + 6  # the two writes of the base, then six appends, each one version
    expected = list(range(200)) + [i for first in (1000, 2000) for i in range(first, first + 300)]
    assert sorted(ds.to_table(columns=["id"])["id"].to_pylist()) == sorted(expected)


def test_add_bases_on_s3_matches_the_local_copy(tmp_path):
    """Lance's UpdateBases (pylance's add_bases): a base directory outside the dataset, registered as a base."""
    from nanolance.lance.base_path import DatasetBasePath

    other = str(tmp_path / "other.lance")
    nl.write_dataset(batch(5000), other)
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    for uri, storage in ((s3_uri, options()), (local, None)):
        _two_fragments(uri, storage)
        nl.dataset(uri, storage_options=storage).add_bases(
            [DatasetBasePath(path=other, name="b1", is_dataset_root=True)])
    assert _snapshot(s3_uri, options()) == _snapshot(local, None)


def _outcome(fn) -> str:
    try:
        fn()
    except (OSError, nl.NotSupportedError) as error:  # the refusal of a commit is the outcome compared here
        return f"{type(error).__name__}: {error}"
    return "ok"


def _conflicting_commits(uri: str, storage) -> list:
    """A Merge and an Append both made from version 2 of the dataset: the second commit is checked against
    the first as on a local copy (an incompatible or retryable conflict, or both commit)."""
    _two_fragments(uri, storage)
    ds = nl.dataset(uri, storage_options=storage)
    merged = [f.merge_columns({"extra": "score + 1"}) for f in ds.get_fragments()]
    schema = merged[0][1]
    outcomes = [_outcome(lambda: _commit_append(uri, storage, [_new_fragment(uri, storage, 300)]))]
    outcomes.append(_outcome(lambda: nl.LanceDataset.commit(
        uri, nl.LanceOperation.Merge([m for m, _ in merged], schema), read_version=ds.version,
        storage_options=storage)))
    return outcomes


def test_conflicting_hand_built_commits_match_the_local_copy(tmp_path):
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    s3_outcomes = _conflicting_commits(s3_uri, options())
    local_outcomes = _conflicting_commits(local, None)
    assert s3_outcomes == local_outcomes
    assert _snapshot(s3_uri, options()) == _snapshot(local, None)



# ── W4: drop, branches and clones on s3:// ───────────────────────────────────────────────────────────

def _keys(uri: str) -> list:
    """Every object key under the prefix of ``uri`` (the dataset directory), by ListObjectsV2 with no
    credentials, the store taking an unsigned listing (moto does; the proxy is bypassed)."""
    import urllib.parse
    import urllib.request
    import xml.etree.ElementTree as ET

    prefix = uri[len(f"s3://{BUCKET}/"):].rstrip("/") + "/"
    url = f"{ENDPOINT}/{BUCKET}?list-type=2&prefix={urllib.parse.quote(prefix)}"
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url, timeout=60) as resp:
        root = ET.fromstring(resp.read())
    assert root.findtext("{*}IsTruncated") == "false", "listing truncated"
    return sorted(key.text for key in root.findall(".//{*}Key"))


def _put_unsigned(uri: str, body: bytes) -> None:
    """One PUT of ``body`` at ``uri`` with no credentials (moto takes it), for an object nanolance did not write."""
    import urllib.request

    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    key = uri[len(f"s3://{BUCKET}/"):]
    request = urllib.request.Request(f"{ENDPOINT}/{BUCKET}/{key}", data=body, method="PUT")
    opener.open(request, timeout=60).close()


def test_drop_on_s3_refuses_a_prefix_with_no_manifest():
    uri = fresh_uri()
    _put_unsigned(uri + "/data/junk.lance", b"not a dataset")
    with pytest.raises(ValueError, match="Refusing to drop"):
        nl.LanceDataset.drop(uri, storage_options=options())
    assert _keys(uri) == [uri[len(f"s3://{BUCKET}/"):] + "/data/junk.lance"]


def _versions_and_rows(view, kinds: bool = True) -> dict:
    """pylance's view of one version line: its versions, each one's rows and (on main) its transaction kind.
    pylance 12 cannot read a branch's transactions (read_transaction panics there, locally as on S3)."""
    out = {}
    for v in (info["version"] for info in view.versions()):
        table = view.checkout_version(v).to_table().sort_by("id").to_pydict()
        out[v] = (type(view.read_transaction(v).operation).__name__, table) if kinds else table
    return out


def _branch_snapshot(uri: str, storage) -> dict:
    """What pylance reads of a dataset's branches (names, forks, metadata, each branch's versions, kinds
    and rows) and of its tags. Branch identifiers (random ids) and create times are left out."""
    lance = require_pylance()
    main = lance.dataset(uri, storage_options=storage)
    branches = {}
    for name, info in main.branches.list().items():
        branches[name] = {"parent_branch": info["parent_branch"], "parent_version": info["parent_version"],
                          "metadata": info["metadata"],
                          "versions": _versions_and_rows(main.checkout_version((name, None)), kinds=False)}
    return {"main": _versions_and_rows(main), "branches": branches,
            "tags": sorted((name, info["version"]) for name, info in main.tags.list().items())}


def _branch_on(root: str, name: str, storage, rows, mode="append") -> None:
    """A write on branch ``name`` of the dataset at ``root``: nanolance's write, at ``<root>/tree/<name>``."""
    nl.write_dataset(rows, f"{root}/tree/{name}", mode=mode, storage_options=storage)


BRANCH_OPS = {
    "create_and_write_and_tag": lambda main, root, storage: (
        main.create_branch("feat"),
        _branch_on(root, "feat", storage, batch(1000, 50)),
        main.tags.create("feat_latest", ("feat", None))),
    "fork_of_a_branch": lambda main, root, storage: (
        main.create_branch("feat"),
        _branch_on(root, "feat", storage, batch(1000, 50)),
        main.create_branch("sub", ("feat", None)),
        _branch_on(root, "sub", storage, batch(2000, 10))),
    "replace_metadata": lambda main, root, storage: (
        main.create_branch("feat"),
        main.branches.replace_metadata("feat", {"owner": "w4"})),
    "overwrite_on_branch": lambda main, root, storage: (
        main.create_branch("feat"),
        _branch_on(root, "feat", storage, batch(3000, 5), mode="overwrite")),
}


@pytest.mark.parametrize("name", list(BRANCH_OPS))
def test_branch_operation_on_s3_matches_the_local_copy(name, tmp_path):
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    for uri, storage in ((s3_uri, options()), (local, None)):
        nl.write_dataset(batch(0), uri, storage_options=storage)
        BRANCH_OPS[name](nl.dataset(uri, storage_options=storage), uri, storage)
    assert _branch_snapshot(s3_uri, options()) == _branch_snapshot(local, None)


def test_branch_delete_on_s3_removes_its_objects_as_locally(tmp_path):
    s3_uri, local = fresh_uri(), str(tmp_path / "ds.lance")
    for uri, storage in ((s3_uri, options()), (local, None)):
        nl.write_dataset(batch(0), uri, storage_options=storage)
        main = nl.dataset(uri, storage_options=storage)
        main.create_branch("gone")
        _branch_on(uri, "gone", storage, batch(1000, 50))
        main.create_branch("kept")
        main.branches.delete("gone")
    assert sorted(_branch_snapshot(s3_uri, options())["branches"]) == ["kept"]
    assert _branch_snapshot(s3_uri, options()) == _branch_snapshot(local, None)
    assert not [k for k in _keys(s3_uri) if "/tree/gone/" in k]
    assert any("/tree/kept/" in k for k in _keys(s3_uri))
    assert not (tmp_path / "ds.lance" / "tree" / "gone").exists()


def test_branch_created_by_pylance_is_written_and_deleted_by_nanolance_on_s3():
    lance = require_pylance()
    uri = fresh_uri()
    main = lance.write_dataset(batch(0), uri, storage_options=options())
    branch = main.create_branch("feat")
    lance.write_dataset(batch(1000, 50), branch.uri, mode="append", storage_options=options())
    nl.write_dataset(batch(2000, 10), f"{uri}/tree/feat", mode="append", storage_options=options())
    theirs = lance.dataset(uri, storage_options=options()).checkout_version(("feat", None))
    assert theirs.count_rows() == 160
    assert sorted(theirs.to_table(columns=["id"])["id"].to_pylist())[-1] == 2009
    nl.dataset(uri, storage_options=options()).branches.delete("feat")
    assert lance.dataset(uri, storage_options=options()).branches.list() == {}
    assert not [k for k in _keys(uri) if "/tree/feat/" in k]


def test_racing_branch_creates_lose_and_double_nothing():
    """Two processes create the same branch: one wins, the other is refused, and the branch reads back."""
    uri = fresh_uri()
    nl.write_dataset(batch(0), uri, storage_options=options())
    racer = textwrap.dedent("""
        import json, sys
        import nanolance.lance as nl
        uri, options = sys.argv[1], json.loads(sys.argv[2])
        try:
            nl.dataset(uri, storage_options=options).create_branch("race")
            print(json.dumps({"created": True}))
        except (OSError, ValueError) as e:  # a loser's manifest publish is a commit conflict
            print(json.dumps({"created": False, "error": str(e)}))
    """)
    procs = [subprocess.Popen([sys.executable, "-c", racer, uri, json.dumps(options())], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True) for _ in range(4)]
    outcomes = [result(p) for p in procs]
    assert sum(o["created"] for o in outcomes) == 1, outcomes
    assert all("already exists" in o["error"] or "committed concurrently" in o["error"]
               for o in outcomes if not o["created"]), outcomes
    lance = require_pylance()
    assert list(lance.dataset(uri, storage_options=options()).branches.list()) == ["race"]


CLONE_KINDS = {"shallow": "shallow_clone", "deep": "deep_clone"}


@pytest.mark.parametrize("kind", list(CLONE_KINDS))
@pytest.mark.parametrize("source_side", ["s3", "local"])
@pytest.mark.parametrize("target_side", ["s3", "local"])
def test_clone_on_s3_reads_as_the_source_version(kind, source_side, target_side, tmp_path):
    lance = require_pylance()
    src_storage = options() if source_side == "s3" else None
    source = fresh_uri() if source_side == "s3" else str(tmp_path / "src.lance")
    nl.write_dataset(batch(0), source, storage_options=src_storage)
    nl.write_dataset(batch(100), source, mode="append", storage_options=src_storage)
    expected = lance.dataset(source, storage_options=src_storage).to_table().sort_by("id").to_pydict()
    target_storage = options() if target_side == "s3" else None
    target = fresh_uri() if target_side == "s3" else str(tmp_path / "clone.lance")
    clone = getattr(nl.dataset(source, storage_options=src_storage), CLONE_KINDS[kind])
    clone(target, 2, storage_options=target_storage)
    theirs = lance.dataset(target, storage_options=options())  # a shallow clone's base path is an s3:// URI
    assert theirs.to_table().sort_by("id").to_pydict() == expected
    assert theirs.count_rows() == 200


def test_deep_clone_on_s3_reads_after_its_source_is_dropped():
    uri, target = fresh_uri(), fresh_uri()
    nl.write_dataset(batch(0), uri, storage_options=options())
    nl.dataset(uri, storage_options=options()).deep_clone(target, 1, storage_options=options())
    nl.LanceDataset.drop(uri, storage_options=options())
    assert not _keys(uri)
    assert nl.dataset(target, storage_options=options()).count_rows() == 100


def test_pylance_clones_on_s3_are_read_by_nanolance():
    lance = require_pylance()
    uri, shallow, deep = fresh_uri(), fresh_uri(), fresh_uri()
    ds = lance.write_dataset(batch(0), uri, storage_options=options())
    ds.shallow_clone(shallow, 1, storage_options=options())
    ds.deep_clone(deep, 1, storage_options=options())
    for target in (shallow, deep):
        mine = nl.dataset(target, storage_options=options())
        assert mine.count_rows() == 100 and mine.version == 1
        assert sorted(mine.to_table(columns=["id"])["id"].to_pylist()) == list(range(100))


def test_drop_on_s3_removes_every_object_of_the_dataset_and_no_sibling():
    uri = fresh_uri()
    sibling = uri + "2"  # a prefix that starts with the dataset's own name
    nl.write_dataset(batch(0), uri, storage_options=options())
    nl.dataset(uri, storage_options=options()).create_branch("feat")
    _branch_on(uri, "feat", options(), batch(1000, 50))
    nl.write_dataset(batch(0), sibling, storage_options=options())
    before = _keys(sibling)
    assert any("/tree/feat/" in k for k in _keys(uri))
    nl.LanceDataset.drop(uri, storage_options=options())
    assert _keys(uri) == []
    assert _keys(sibling) == before
    with pytest.raises(OSError, match="was not found"):
        nl.LanceDataset.drop(uri, storage_options=options())
    nl.LanceDataset.drop(uri, storage_options=options(), ignore_not_found=True)


# ── cleanup_old_versions on s3:// ────────────────────────────────────────────────────────────────


def _history(uri: str, storage) -> None:
    """Versions 1-6 with every kind of file cleanup decides on: data, deletions, an index, transactions."""
    nl.write_dataset(batch(0), uri, storage_options=storage)
    nl.write_dataset(batch(100), uri, mode="append", storage_options=storage)
    nl.dataset(uri, storage_options=storage).delete("id < 50")
    nl.dataset(uri, storage_options=storage).create_scalar_index("id", "BTREE")
    nl.write_dataset(batch(200), uri, mode="append", storage_options=storage)
    nl.dataset(uri, storage_options=storage).update({"id": "id + 1000"}, where="id >= 250")
    nl.dataset(uri, storage_options=storage).optimize.compact_files()  # old data and deletion files unreferenced


def _local_files(path) -> list:
    # On NFS a deleted file that is still open is renamed to .nfsXXXX until it is closed (nanolance keeps a
    # few data files open per thread): not a file of the dataset.
    return sorted(str(p.relative_to(path)) for p in path.rglob("*") if p.is_file() and not p.name.startswith(".nfs"))


def _relative_keys(uri: str) -> list:
    prefix = uri[len(f"s3://{BUCKET}/"):].rstrip("/") + "/"
    return [k[len(prefix):] for k in _keys(uri)]


def _shape(names: list) -> list:
    """The top directory of each file: names are random, so two datasets agree on how many files each holds."""
    return sorted(n.split("/")[0] for n in names)


CLEANUPS = {
    "retain_versions": lambda ds: ds.cleanup_old_versions(retain_versions=2),
    "older_than_unverified": lambda ds: ds.cleanup_old_versions(older_than=timedelta(0), delete_unverified=True),
    "older_than_verified_only": lambda ds: ds.cleanup_old_versions(older_than=timedelta(0)),
    "tagged_kept": lambda ds: (ds.tags.create("keep", 2),
                               ds.cleanup_old_versions(older_than=timedelta(0), delete_unverified=True,
                                                       error_if_tagged_old_versions=False))[1],
}


@pytest.mark.parametrize("name", list(CLEANUPS))
def test_cleanup_on_s3_matches_the_local_copy(name, tmp_path):
    s3_uri, local = fresh_uri(), tmp_path / "ds.lance"
    stats = []
    for uri, storage in ((s3_uri, options()), (str(local), None)):
        _history(uri, storage)
        stats.append(CLEANUPS[name](nl.dataset(uri, storage_options=storage)))
    # Every count equal (bytes may not be: a manifest's size differs by a byte or two between locations).
    counted = ("old_versions", "data_files_removed", "transaction_files_removed", "index_files_removed",
               "deletion_files_removed")
    assert [getattr(stats[0], c) for c in counted] == [getattr(stats[1], c) for c in counted]
    assert stats[0].old_versions > 0 and stats[0].bytes_removed > 0
    assert _shape(_relative_keys(s3_uri)) == _shape(_local_files(local))
    assert len(_relative_keys(s3_uri)) == len(_local_files(local))
    ours, theirs = nl.dataset(s3_uri, storage_options=options()), nl.dataset(str(local))
    assert [v["version"] for v in ours.versions()] == [v["version"] for v in theirs.versions()]
    for v in ours.versions():
        got = nl.dataset(s3_uri, version=v["version"], storage_options=options()).to_table().sort_by("id")
        assert got.equals(nl.dataset(str(local), version=v["version"]).to_table().sort_by("id"))


def test_pylance_reads_what_nanolance_cleaned_on_s3():
    lance = require_pylance()
    uri = fresh_uri()
    _history(uri, options())
    ours = nl.dataset(uri, storage_options=options())
    before, latest = ours.to_table().sort_by("id"), ours.version
    ours.cleanup_old_versions(older_than=timedelta(0), delete_unverified=True)
    theirs = lance.dataset(uri, storage_options=options())
    assert [v["version"] for v in theirs.versions()] == [latest]
    assert theirs.to_table().sort_by("id").equals(before)
    assert theirs.list_indices()[0]["fields"] == ["id"]


def test_nanolance_cleanup_of_a_pylance_dataset_on_s3_matches_pylance_cleanup():
    lance = require_pylance()
    ours_uri, theirs_uri = fresh_uri(), fresh_uri()
    for uri in (ours_uri, theirs_uri):
        lance.write_dataset(batch(0), uri, storage_options=options())
        lance.write_dataset(batch(100), uri, mode="append", storage_options=options())
        lance.dataset(uri, storage_options=options()).delete("id < 50")
        lance.write_dataset(batch(200), uri, mode="append", storage_options=options())
    ours = nl.dataset(ours_uri, storage_options=options()).cleanup_old_versions(
        older_than=timedelta(0), delete_unverified=True)
    theirs = lance.dataset(theirs_uri, storage_options=options()).cleanup_old_versions(
        older_than=timedelta(0), delete_unverified=True)
    assert ours.old_versions == theirs.old_versions == 3
    assert _shape(_relative_keys(ours_uri)) == _shape(_relative_keys(theirs_uri))
    assert lance.dataset(ours_uri, storage_options=options()).to_table().sort_by("id").equals(
        lance.dataset(theirs_uri, storage_options=options()).to_table().sort_by("id"))


def test_cleanup_on_s3_keeps_what_a_branch_forked_from(tmp_path):
    s3_uri, local = fresh_uri(), tmp_path / "ds.lance"
    for uri, storage in ((s3_uri, options()), (str(local), None)):
        nl.write_dataset(batch(0), uri, storage_options=storage)
        nl.write_dataset(batch(100), uri, mode="append", storage_options=storage)
        nl.dataset(uri, storage_options=storage).create_branch("exp", 2)
        nl.write_dataset(batch(200), uri, mode="overwrite", storage_options=storage)
        nl.write_dataset(batch(300), uri, mode="append", storage_options=storage)
        nl.dataset(uri, storage_options=storage).cleanup_old_versions(older_than=timedelta(0),
                                                                      delete_unverified=True)
    assert _shape(_relative_keys(s3_uri)) == _shape(_local_files(local))
    branch = nl.dataset(f"{s3_uri}/tree/exp", storage_options=options())
    assert branch.count_rows() == 200


# ── races between the two engines on s3:// ────────────────────────────────────────────────────────

ENGINE_CHILD = textwrap.dedent("""
    import json, os, sys
    if os.environ.get("NANOLANCE_TEST_CHILD_PRELUDE"):
        exec(open(os.environ["NANOLANCE_TEST_CHILD_PRELUDE"]).read())
    import pyarrow as pa
    engine, op, uri, first, n, options = sys.argv[1:7]
    first, n, options = int(first), int(n), json.loads(options)
    if engine == "pylance":
        import lance as m
    else:
        import nanolance.lance as m
    ok, errors = 0, []
    for i in range(n):
        start = first + i * 100
        try:
            if op == "append":
                t = pa.table({"id": pa.array(range(start, start + 100), pa.int64()),
                              "name": pa.array([f"row {j}" for j in range(start, start + 100)])})
                m.write_dataset(t, uri, mode="append", storage_options=options)
            else:
                m.dataset(uri, storage_options=options).delete(f"id >= {start} AND id < {start + 50}")
            ok += 1
        except (OSError, ValueError) as e:
            errors.append(str(e))
    print(json.dumps({"ok": ok, "errors": errors}))
""")


def start_engine(engine: str, op: str, uri: str, first: int, n: int) -> subprocess.Popen:
    return subprocess.Popen([sys.executable, "-c", ENGINE_CHILD, engine, op, uri, str(first), str(n),
                             json.dumps(options())], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def _every_version_opens_in_both(uri: str, lance) -> None:
    latest = nl.dataset(uri, storage_options=options()).version
    for v in range(1, latest + 1):
        assert nl.dataset(uri, version=v, storage_options=options()).count_rows() == \
            lance.dataset(uri, version=v, storage_options=options()).count_rows()


def test_pylance_and_nanolance_racing_appends_lose_and_double_nothing():
    lance = require_pylance()
    uri = fresh_uri()
    nl.write_dataset(batch(0), uri, storage_options=options())
    procs = [start_engine("nanolance", "append", uri, 10_000, 5), start_engine("pylance", "append", uri, 50_000, 5)]
    outcomes = [result(p) for p in procs]
    assert [o["ok"] for o in outcomes] == [5, 5], outcomes
    ids = sorted(nl.dataset(uri, storage_options=options()).to_table(columns=["id"])["id"].to_pylist())
    assert ids == sorted(list(range(100)) + [s + j for s in (10_000, 50_000) for j in range(500)])
    assert nl.dataset(uri, storage_options=options()).version == 11
    _every_version_opens_in_both(uri, lance)


def test_nanolance_deletes_racing_pylance_appends_on_s3():
    lance = require_pylance()
    uri = fresh_uri()
    nl.write_dataset(batch(0, 1000), uri, storage_options=options())
    procs = [start_engine("nanolance", "delete", uri, 0, 5), start_engine("pylance", "append", uri, 50_000, 5)]
    outcomes = [result(p) for p in procs]
    assert [o["ok"] for o in outcomes] == [5, 5], outcomes
    deleted = {j for s in range(0, 500, 100) for j in range(s, s + 50)}
    expected = sorted([i for i in range(1000) if i not in deleted] + list(range(50_000, 50_500)))
    for ds in (nl.dataset(uri, storage_options=options()), lance.dataset(uri, storage_options=options())):
        assert sorted(ds.to_table(columns=["id"])["id"].to_pylist()) == expected
    _every_version_opens_in_both(uri, lance)


def test_auto_cleanup_on_commit_on_s3_as_pylance():
    """Lance's auto cleanup hook runs after a commit on an object store too (it once ran only locally)."""
    lance = require_pylance()
    versions = []
    for engine in (nl, lance):
        uri = fresh_uri()
        engine.write_dataset(pa.table({"a": [0]}), uri, storage_options=options(),
                             auto_cleanup_options={"interval": 2, "older_than_seconds": 0})
        for i in range(1, 5):
            engine.write_dataset(pa.table({"a": [i]}), uri, mode="append", storage_options=options())
        versions.append([v["version"] for v in engine.dataset(uri, storage_options=options()).versions()])
        assert sorted(nl.dataset(uri, storage_options=options()).to_table()["a"].to_pylist()) == [0, 1, 2, 3, 4]
    assert versions[0] == versions[1] == [3, 4, 5]


def test_io_stats_count_what_a_write_to_s3_puts(tmp_path):
    """Written bytes and write operations on s3:// are counted as on a local copy (the local tally stats
    finished files, which an object store does not have; the S3 sink counts each upload)."""
    stats = []
    for uri, storage in ((fresh_uri(), options()), (str(tmp_path / "ds.lance"), None)):
        nl.write_dataset(batch(0, 1000), uri, storage_options=storage)
        ds = nl.dataset(uri, storage_options=storage)
        ds.io_stats_incremental()
        ds.delete("id % 7 = 0")
        stats.append(ds.io_stats_incremental())
    s3, local = stats
    assert s3.write_iops == local.write_iops > 0
    assert abs(s3.written_bytes - local.written_bytes) <= 16 and s3.written_bytes > 0  # manifests differ by a byte or two
