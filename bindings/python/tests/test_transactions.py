"""Transaction files: every nanolance commit leaves one, as Lance's commits do.

pylance reads the transactions of the versions committed since its read version to decide whether
its own change still applies; a version without one made its commit fail outright ("Dataset version
N does not have a transaction file"). These tests run pylance commits from a stale read version
over each kind of nanolance commit, and check that pylance decodes each transaction as the
operation it would itself have recorded.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


def _table(n=100):
    return pa.table({"id": range(n), "v": [float(i) for i in range(n)]})


# Each nanolance change, and the operation pylance records for the same change.
CHANGES = {
    "append": (lambda p: nl.write_dataset(_table(10), p, mode="append"), "Append"),
    "delete": (lambda p: nl.dataset(p).delete("id < 5"), "Delete"),
    "update": (lambda p: nl.dataset(p).update({"v": "v * 2"}, where="id > 90"), "Update"),
    "merge_insert": (lambda p: nl.dataset(p).merge_insert("id").when_matched_update_all()
                     .when_not_matched_insert_all().execute(pa.table({"id": [1, 500], "v": [0.5, 0.25]})), "Update"),
    "insert_only_merge": (lambda p: nl.dataset(p).merge_insert("id").when_not_matched_insert_all()
                          .execute(pa.table({"id": [600], "v": [1.0]})), "Update"),
    "index": (lambda p: nl.dataset(p).create_scalar_index("id", "BTREE"), "CreateIndex"),
    "config": (lambda p: nl.dataset(p).update_config({"k": "v"}), "UpdateConfig"),
    "add_column": (lambda p: nl.dataset(p).add_columns({"w": "v + 1"}), "Merge"),
    "overwrite": (lambda p: nl.write_dataset(_table(7), p, mode="overwrite"), "Overwrite"),
    "compaction": (lambda p: nl.dataset(p).optimize.compact_files(target_rows_per_fragment=1000, reindex=False),
                   "Rewrite"),
}


@pytest.mark.parametrize("change", sorted(CHANGES))
def test_pylance_reads_the_transaction(tmp_path, change):
    lance = require_pylance()
    path = str(tmp_path / "d.lance")
    nl.write_dataset(_table(), path, max_rows_per_file=40)
    run, expected = CHANGES[change]
    run(path)
    ds = lance.dataset(path)
    tx = ds.read_transaction(ds.version)
    assert tx is not None and type(tx.operation).__name__ == expected
    assert tx.read_version == ds.version - 1
    assert type(ds.read_transaction(1).operation).__name__ == "Overwrite"


@pytest.mark.parametrize("change", sorted(CHANGES))
@pytest.mark.parametrize("theirs", ["insert", "delete"])
def test_pylance_commits_over_a_nanolance_commit(tmp_path, change, theirs):
    """pylance read version 1; nanolance committed version 2; pylance then commits. It must either
    land (its change applied on top of nanolance's) or fail with a commit conflict -- never with
    Lance's 'does not have a transaction file' internal error."""
    lance = require_pylance()
    path = str(tmp_path / "d.lance")
    nl.write_dataset(_table(), path, max_rows_per_file=40)
    stale = lance.dataset(path)
    CHANGES[change][0](path)
    between = nl.dataset(path).to_table().sort_by("id")
    try:
        if theirs == "insert":
            stale.insert(pa.table({"id": [1000], "v": [9.0]}))
        else:
            stale.delete("id = 50")
    except OSError as exc:
        assert "transaction file" not in str(exc) and "internal error" not in str(exc).lower()
        assert "conflict" in str(exc).lower() or "retry" in str(exc).lower()
        return
    after = lance.dataset(path).to_table().sort_by("id")
    assert nl.dataset(path).to_table().sort_by("id").equals(after)
    if theirs == "insert":
        expected = pa.concat_tables([between, pa.table({"id": [1000], "v": [9.0]}).cast(between.schema)
                                     if between.num_columns == 2 else
                                     pa.table({"id": [1000], "v": [9.0], "w": [None]}).cast(between.schema)])
    else:
        expected = between.filter(pa.compute.not_equal(between["id"], 50))
    assert after.equals(expected.sort_by("id"))


def test_cleanup_counts_transactions(tmp_path):
    path = str(tmp_path / "c.lance")
    for mode in ("create", "overwrite", "overwrite", "append"):
        nl.write_dataset(_table(), path, mode=mode)
    stats = nl.dataset(path).cleanup_old_versions(retain_versions=3)
    assert (stats.old_versions, stats.data_files_removed, stats.transaction_files_removed) == (1, 1, 1)


def test_one_transaction_per_version(tmp_path):
    """Commits that lose a race retry with a new transaction; the loser's file goes, so every file
    left belongs to a version."""
    import os
    import threading

    path = str(tmp_path / "f.lance")
    nl.write_dataset(_table(), path)

    def worker(k):
        for i in range(5):
            nl.write_dataset(pa.table({"id": [10_000 + 10 * k + i], "v": [0.0]}), path, mode="append")

    threads = [threading.Thread(target=worker, args=(k,)) for k in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    versions = [v["version"] for v in nl.dataset(path).versions()]
    assert len(versions) == 21 and nl.dataset(path).count_rows() == 120
    files = sorted(os.listdir(os.path.join(path, "_transactions")))
    assert len(files) == len(versions)
    lance = require_pylance()
    ds = lance.dataset(path)
    assert sorted(ds.read_transaction(v).read_version for v in versions) == [v - 1 for v in versions]
