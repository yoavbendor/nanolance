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
