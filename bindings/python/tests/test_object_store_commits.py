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

import pyarrow as pa
import pytest

from tests.support import require_pylance

import nanolance.lance as nl

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

