#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise everything nanolance does on S3, against a real bucket, and report what works.

    python3 tools/s3_real_check.py s3://my-bucket/some/scratch/prefix [options]

Each check runs under its own fresh sub-prefix of the one given, so one failure does not hide the
rest, and prints PASS / FAIL / SKIP with the error. At the end a table and a JSON report
(--report, default s3_real_check_report.json) say what works and what needs more work.

Credentials, region and endpoint come from the usual AWS chain (AWS_ACCESS_KEY_ID /
AWS_SECRET_ACCESS_KEY / AWS_SESSION_TOKEN, AWS_PROFILE and ~/.aws, ECS / EC2 roles; AWS_REGION) --
nothing is passed on the command line or written to the report. --endpoint / --region / --allow-http
are for S3-compatible stores (MinIO, R2, ...). The prefix must be writable and should be empty: the
script writes small datasets under it and, unless --keep is given, deletes everything it wrote.

pylance (`pip install pylance`) is optional: with it the script also checks that pylance reads what
nanolance wrote and the reverse; without it those checks are SKIPped.

Options:
  --only NAME[,NAME]   run only these checks (names as in the table)
  --large              also write and read back a ~100 MiB dataset (multipart uploads)
  --faults             also run the fault-injection checks (a lost reply of the manifest PUT), which use
                       nanos3reader's test hook in a child process
  --keep               leave the datasets in the bucket (to inspect them)
"""

import argparse
import json
import os
import subprocess
import sys
import textwrap
import time
import traceback
import uuid

import pyarrow as pa

import nanolance.lance as nl
from nanolance.lance.fragment import write_fragments

try:
    import lance as pylance
except ImportError:  # optional
    pylance = None

CHECKS = []


def check(name, needs_pylance=False, optional=None):
    def register(fn):
        CHECKS.append((name, fn, needs_pylance, optional))
        return fn
    return register


class Skip(Exception):
    pass


# ── helpers ─────────────────────────────────────────────────────────────────────────────────────

def table(start=0, rows=1000):
    ids = list(range(start, start + rows))
    return pa.table({
        "id": pa.array(ids, pa.int64()),
        "cat": pa.array([f"c{i % 7}" for i in ids]),
        "text": pa.array([f"alpha beta gamma{i % 13} delta" for i in ids]),
        "tags": pa.array([[f"t{i % 5}", f"t{i % 3}"] for i in ids], pa.list_(pa.string())),
        "score": pa.array([float(i % 101) / 7.0 for i in ids], pa.float64()),
        "vec": pa.FixedSizeListArray.from_arrays(
            pa.array([((i * 7 + j * 13) % 97) / 97.0 for i in ids for j in range(16)], pa.float32()), 16),
    })


def ids_of(ds):
    return sorted(ds.to_table(columns=["id"])["id"].to_pylist())


def expect(cond, what):
    if not cond:
        raise AssertionError(what)


class Ctx:
    def __init__(self, args):
        self.root = args.prefix.rstrip("/")
        self.options = {}
        if args.endpoint:
            self.options["aws_endpoint"] = args.endpoint
        if args.region:
            self.options["aws_region"] = args.region
        if args.allow_http:
            self.options["allow_http"] = "true"
        self.run_id = time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
        self.written = []

    def uri(self, name):
        u = f"{self.root}/nanolance-s3-check-{self.run_id}/{name}.lance"
        self.written.append(u)
        return u

    def nl_write(self, data, uri, **kw):
        return nl.write_dataset(data, uri, storage_options=self.options, **kw)

    def nl_open(self, uri, **kw):
        return nl.dataset(uri, storage_options=self.options, **kw)

    def py_open(self, uri, **kw):
        return pylance.dataset(uri, storage_options=self.options or None, **kw)

    def py_write(self, data, uri, **kw):
        return pylance.write_dataset(data, uri, storage_options=self.options or None, **kw)


# ── checks ──────────────────────────────────────────────────────────────────────────────────────

@check("create_append_overwrite")
def _(c):
    u = c.uri("basic")
    expect(c.nl_write(table(0), u).version == 1, "create is version 1")
    expect(c.nl_write(table(1000), u, mode="append").count_rows() == 2000, "append adds 1000 rows")
    try:
        c.nl_write(table(0), u, mode="create")
        raise AssertionError("create over an existing dataset was accepted")
    except OSError:
        pass
    ds = c.nl_write(table(0, 10), u, mode="overwrite")
    expect(ds.version == 3 and ds.count_rows() == 10, "overwrite: version 3, 10 rows")
    expect(c.nl_open(u, version=2).count_rows() == 2000, "version 2 still reads 2000 rows")


@check("reads")
def _(c):
    u = c.uri("reads")
    c.nl_write(table(0, 3000), u, max_rows_per_file=1000)
    ds = c.nl_open(u)
    expect(ds.count_rows() == 3000, "count_rows")
    expect(ds.to_table(columns=["id", "cat"]).num_columns == 2, "projection")
    expect(ds.count_rows(filter="cat = 'c3'") == sum(1 for i in range(3000) if i % 7 == 3), "filter")
    expect(ds.take([0, 1500, 2999], columns=["id"])["id"].to_pylist() == [0, 1500, 2999], "take")
    expect(len(ds.get_fragments()) == 3, "3 fragments")
    expect([v["version"] for v in ds.versions()] == [1], "versions()")


@check("pylance_reads_nanolance", needs_pylance=True)
def _(c):
    u = c.uri("nl_to_py")
    c.nl_write(table(0), u)
    c.nl_write(table(1000), u, mode="append")
    p = c.py_open(u)
    expect(p.version == 2 and p.count_rows() == 2000, "pylance: version 2, 2000 rows")
    expect(p.to_table().to_pydict() == c.nl_open(u).to_table().to_pydict(), "same table in both engines")


@check("nanolance_appends_to_pylance", needs_pylance=True)
def _(c):
    u = c.uri("py_to_nl")
    c.py_write(table(0), u)
    c.nl_write(table(1000), u, mode="append")
    c.py_write(table(2000), u, mode="append")
    expect(ids_of(c.nl_open(u)) == list(range(3000)), "nanolance reads all three appends")
    expect(c.py_open(u).count_rows() == 3000, "pylance reads all three appends")


@check("delete_update_merge_insert")
def _(c):
    u = c.uri("mutations")
    c.nl_write(table(0), u)
    ds = c.nl_open(u)
    ds.delete("id < 100")
    ds = c.nl_open(u)
    ds.update({"score": "-1.0"}, where="id >= 900")
    ds = c.nl_open(u)
    ds.merge_insert("id").when_matched_update_all().when_not_matched_insert_all().execute(table(950, 100))
    ds = c.nl_open(u)
    expect(ids_of(ds) == list(range(100, 1050)), "rows after delete + upsert")
    expect(ds.count_rows(filter="score = -1.0") == 50, "the update kept on rows not upserted")
    if pylance is not None:
        expect(c.py_open(u).count_rows() == 950, "pylance agrees")


@check("restore_config_metadata_tags")
def _(c):
    u = c.uri("refs")
    c.nl_write(table(0), u)
    c.nl_write(table(1000), u, mode="append")
    ds = c.nl_open(u)
    ds.tags.create("first", 1)
    expect(ds.tags.list()["first"]["version"] == 1, "tag created")
    ds.update_config({"team.owner": "s3-check"})
    ds.update_metadata({"note": "hello"})
    ds = c.nl_open(u, version="first")
    expect(ds.count_rows() == 1000, "checkout by tag")
    ds.restore()
    ds = c.nl_open(u)
    expect(ds.count_rows() == 1000, "restore made version 1 the latest")
    ds.tags.delete("first")
    expect("first" not in c.nl_open(u).tags.list(), "tag deleted")


@check("schema_changes")
def _(c):
    u = c.uri("schema")
    c.nl_write(table(0), u)
    ds = c.nl_open(u)
    ds.add_columns({"double_id": "id * 2"})
    ds = c.nl_open(u)
    ds.alter_columns({"path": "double_id", "name": "twice"})
    ds = c.nl_open(u)
    ds.drop_columns(["score"])
    ds = c.nl_open(u)
    names = ds.schema.names
    expect("twice" in names and "score" not in names, f"schema after changes: {names}")
    expect(ds.to_table(columns=["twice"])["twice"].to_pylist()[:3] == [0, 2, 4], "computed column")


@check("compaction")
def _(c):
    u = c.uri("compact")
    for i in range(5):
        c.nl_write(table(i * 200, 200), u, mode="append" if i else "create")
    ds = c.nl_open(u)
    ds.delete("id % 10 = 0")
    before = ids_of(c.nl_open(u))
    c.nl_open(u).optimize.compact_files(target_rows_per_fragment=10_000)
    ds = c.nl_open(u)
    expect(ids_of(ds) == before, "same rows after compaction")
    expect(len(ds.get_fragments()) == 1, f"one fragment after compaction ({len(ds.get_fragments())})")


@check("scalar_indexes")
def _(c):
    u = c.uri("scalar")
    c.nl_write(table(0, 5000), u)
    ds = c.nl_open(u)
    ds.create_scalar_index("id", "BTREE")
    ds.create_scalar_index("cat", "BITMAP")
    ds.create_scalar_index("tags", "LABEL_LIST")
    ds = c.nl_open(u)
    expect(len(ds.list_indices()) == 3, "three indexes")
    expect(ds.count_rows(filter="id = 4321") == 1, "BTREE lookup")
    expect(ds.count_rows(filter="cat = 'c2'") == sum(1 for i in range(5000) if i % 7 == 2), "BITMAP lookup")
    expect(ds.count_rows(filter="array_has_any(tags, ['t4'])") == sum(1 for i in range(5000) if i % 5 == 4),
           "LABEL_LIST lookup")
    plan = ds.scanner(filter="id = 4321").explain_plan()
    expect("ScalarIndexQuery" in plan, "the plan uses the index")
    if pylance is not None:
        expect(c.py_open(u).count_rows(filter="id = 4321") == 1, "pylance uses nanolance's index")


@check("full_text_index")
def _(c):
    u = c.uri("fts")
    c.nl_write(table(0, 3000), u)
    ds = c.nl_open(u)
    ds.create_scalar_index("text", "INVERTED")
    ds = c.nl_open(u)
    hits = ds.to_table(full_text_query="gamma7", columns=["id"])["id"].to_pylist()
    expect(sorted(hits) == [i for i in range(3000) if i % 13 == 7][:len(hits)] and hits, "FTS hits")
    if pylance is not None:
        theirs = c.py_open(u).to_table(full_text_query="gamma7", columns=["id"])["id"].to_pylist()
        expect(sorted(theirs) == sorted(hits), "pylance gets the same hits")


@check("vector_indexes")
def _(c):
    u = c.uri("vector")
    c.nl_write(table(0, 4000), u)
    ds = c.nl_open(u)
    ds.create_index("vec", index_type="IVF_PQ", num_partitions=8, num_sub_vectors=4)
    ds = c.nl_open(u)
    q = [0.5] * 16
    got = ds.to_table(nearest={"column": "vec", "q": q, "k": 10}, columns=["id"])
    expect(got.num_rows == 10, "10 neighbours")
    exact = ds.to_table(nearest={"column": "vec", "q": q, "k": 10, "use_index": False}, columns=["id"])
    expect(exact.num_rows == 10, "flat search")
    ds.create_index("vec", index_type="IVF_FLAT", num_partitions=4, name="vec_flat", replace=True)
    if pylance is not None:
        expect(c.py_open(u).to_table(nearest={"column": "vec", "q": q, "k": 5}).num_rows == 5,
               "pylance searches nanolance's index")


@check("index_maintenance")
def _(c):
    u = c.uri("idx_maint")
    c.nl_write(table(0, 2000), u)
    ds = c.nl_open(u)
    ds.create_scalar_index("id", "BTREE")
    c.nl_write(table(2000, 500), u, mode="append")
    c.nl_open(u).optimize.optimize_indices()
    ds = c.nl_open(u)
    expect(ds.count_rows(filter="id = 2100") == 1, "the appended row is found")
    ds.drop_index("id_idx")
    expect(c.nl_open(u).list_indices() == [], "index dropped")


@check("fragments_and_commits")
def _(c):
    u = c.uri("fragments")
    c.nl_write(table(0), u)
    v = c.nl_open(u).version
    frag = nl.LanceFragment.create(u, table(1000, 100), storage_options=c.options)
    nl.LanceDataset.commit(u, nl.LanceOperation.Append([frag]), read_version=v, storage_options=c.options)
    frags = write_fragments(table(1100, 200), u, storage_options=c.options)
    nl.LanceDataset.commit(u, nl.LanceOperation.Append(frags), read_version=v + 1, storage_options=c.options)
    expect(ids_of(c.nl_open(u)) == list(range(1300)), "rows of both hand-built commits")


@check("cleanup_old_versions")
def _(c):
    from datetime import timedelta
    u = c.uri("cleanup")
    for i in range(4):
        c.nl_write(table(0, 100), u, mode="overwrite" if i else "create")
    stats = c.nl_open(u).cleanup_old_versions(older_than=timedelta(0), delete_unverified=True)
    ds = c.nl_open(u)
    expect(stats.old_versions == 3, f"3 old versions removed ({stats.old_versions})")
    expect([v["version"] for v in ds.versions()] == [4] and ds.count_rows() == 100, "latest still reads")


@check("branches")
def _(c):
    u = c.uri("branches")
    c.nl_write(table(0, 100), u)
    ds = c.nl_open(u)
    br = ds.create_branch("exp")
    c.nl_write(table(100, 50), br.uri, mode="append")
    expect(c.nl_open(u).count_rows() == 100, "main untouched")
    expect(c.nl_open(u).checkout_version(("exp", None)).count_rows() == 150, "branch has the append")
    if pylance is not None:
        expect(c.py_open(u).checkout_version(("exp", None)).count_rows() == 150, "pylance reads the branch")
    c.nl_open(u).branches.delete("exp")
    expect("exp" not in c.nl_open(u).branches.list(), "branch deleted")


@check("clones")
def _(c):
    src = c.uri("clone_src")
    c.nl_write(table(0, 300), src)
    ds = c.nl_open(src)
    shallow = ds.shallow_clone(c.uri("clone_shallow"), 1, storage_options=c.options)
    deep = ds.deep_clone(c.uri("clone_deep"), 1, storage_options=c.options)
    expect(shallow.count_rows() == 300 and deep.count_rows() == 300, "both clones read")
    c.nl_write(table(300, 10), deep.uri, mode="append")
    expect(c.nl_open(src).count_rows() == 300, "the source is untouched by a write to the deep clone")


@check("drop")
def _(c):
    u = c.uri("drop")
    c.nl_write(table(0, 100), u)
    nl.LanceDataset.drop(u, storage_options=c.options)
    try:
        c.nl_open(u)
        raise AssertionError("a dropped dataset still opens")
    except (OSError, ValueError):
        pass


@check("blob_columns")
def _(c):
    u = c.uri("blobs")
    blobs = [os.urandom(n) for n in (100, 70_000, 5_000_000)]  # inline, packed, dedicated
    t = pa.table({"id": pa.array([0, 1, 2], pa.int64()), "blob": nl.blob_array(blobs)})
    c.nl_write(t, u)
    files = c.nl_open(u).take_blobs("blob", indices=[0, 1, 2])
    expect([f.read() for f in files] == blobs, "blobs read back")


CHILD = textwrap.dedent("""
    import json, sys
    import pyarrow as pa
    import nanolance.lance as nl
    uri, first, n, options = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), json.loads(sys.argv[4])
    ok, errors = 0, []
    for i in range(n):
        s = first + i * 100
        t = pa.table({"id": pa.array(range(s, s + 100), pa.int64())})
        try:
            nl.write_dataset(t, uri, mode="append", storage_options=options); ok += 1
        except OSError as e:
            errors.append(str(e))
    print(json.dumps({"ok": ok, "errors": errors}))
""")


def child(c, uri, first, n, faults=""):
    env = dict(os.environ, NANOS3READER_FAULTS=faults)
    return subprocess.Popen([sys.executable, "-c", CHILD, uri, str(first), str(n), json.dumps(c.options)],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def outcome(p):
    out, err = p.communicate(timeout=1800)
    if p.returncode != 0:
        raise AssertionError(f"child failed: {err.strip()[-400:]}")
    return json.loads(out.strip().splitlines()[-1])


@check("racing_writers")
def _(c):
    u = c.uri("race")
    nl.write_dataset(pa.table({"id": pa.array(range(100), pa.int64())}), u, storage_options=c.options)
    procs = [child(c, u, 1000, 5), child(c, u, 100_000, 5)]
    res = [outcome(p) for p in procs]
    expect(all(r["ok"] == 5 for r in res), f"every append landed: {res}")
    got = ids_of(c.nl_open(u))
    expected = sorted(list(range(100)) + [s + j for s in (1000, 100_000) for i in range(5)
                                          for j in range(i * 100, i * 100 + 100)])
    expect(got == expected, "no row lost or doubled")
    expect(c.nl_open(u).version == 11, "11 versions")


@check("lost_reply_of_manifest_put", optional="faults")
def _(c):
    u = c.uri("lost_reply")
    nl.write_dataset(pa.table({"id": pa.array(range(100), pa.int64())}), u, storage_options=c.options)
    r = outcome(child(c, u, 1000, 1, faults="lost:PUT:.manifest:1"))
    expect(r == {"ok": 1, "errors": []}, f"the append committed: {r}")
    ds = c.nl_open(u)
    expect(ds.version == 2 and ds.count_rows() == 200, "exactly one new version, no duplicate")


@check("large_multipart", optional="large")
def _(c):
    u = c.uri("large")
    rows = 1_600_000
    t = pa.table({"id": pa.array(range(rows), pa.int64()),
                  "payload": pa.array([os.urandom(48) for _ in range(rows)], pa.binary())})
    c.nl_write(t, u)
    ds = c.nl_open(u)
    expect(ds.count_rows() == rows, "all rows")
    expect(ds.take([rows - 1], columns=["id"])["id"].to_pylist() == [rows - 1], "last row")


# ── runner ──────────────────────────────────────────────────────────────────────────────────────

def cleanup(c):
    for u in c.written:
        try:
            nl.LanceDataset.drop(u, storage_options=c.options, ignore_not_found=True)
        except Exception as e:  # report and keep going: a leftover is listed, not fatal
            print(f"  (could not remove {u}: {e})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefix", help="s3://bucket/prefix to write under (should be empty)")
    ap.add_argument("--endpoint")
    ap.add_argument("--region")
    ap.add_argument("--allow-http", action="store_true")
    ap.add_argument("--only")
    ap.add_argument("--large", action="store_true")
    ap.add_argument("--faults", action="store_true")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--report", default="s3_real_check_report.json")
    args = ap.parse_args()
    if not args.prefix.startswith("s3://"):
        ap.error("the prefix must be an s3:// URI")
    c = Ctx(args)
    only = set(args.only.split(",")) if args.only else None
    print(f"nanolance S3 check: {c.root}/nanolance-s3-check-{c.run_id}/  "
          f"(pylance {'%s' % pylance.__version__ if pylance else 'not installed'})")
    results = []
    for name, fn, needs_pylance, optional in CHECKS:
        if only and name not in only:
            continue
        t0 = time.time()
        if needs_pylance and pylance is None:
            status, detail = "SKIP", "pylance not installed"
        elif optional and not getattr(args, optional):
            status, detail = "SKIP", f"needs --{optional}"
        else:
            try:
                fn(c)
                status, detail = "PASS", ""
            except Skip as e:
                status, detail = "SKIP", str(e)
            except Exception as e:  # a diagnostic tool: every failure is reported, none stops the run
                status, detail = "FAIL", f"{type(e).__name__}: {e}"
                if os.environ.get("S3_CHECK_TRACEBACK"):
                    traceback.print_exc()
        secs = time.time() - t0
        results.append({"check": name, "status": status, "seconds": round(secs, 2), "detail": detail})
        print(f"  {status:4s} {name:32s} {secs:7.2f}s  {detail[:160]}")
        sys.stdout.flush()
    if not args.keep:
        cleanup(c)
    counts = {s: sum(1 for r in results if r["status"] == s) for s in ("PASS", "FAIL", "SKIP")}
    print(f"\n{counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIP']} skipped")
    with open(args.report, "w") as f:
        json.dump({"prefix": c.root, "run_id": c.run_id, "endpoint": args.endpoint or "aws default",
                   "pylance": getattr(pylance, "__version__", None), "results": results}, f, indent=2)
    print(f"report: {os.path.abspath(args.report)}")
    sys.exit(1 if counts["FAIL"] else 0)


if __name__ == "__main__":
    main()
