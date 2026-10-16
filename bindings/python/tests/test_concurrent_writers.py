"""Writers racing on one dataset, from several processes and threads, as Lance allows.

A commit that loses its version to another writer is not refused: an append (which reads nothing
of the dataset but its schema) is built again on the newer version; a change made from an older
version is rebased onto it when the other writers only added fragments; a delete, update, merge or
compaction whose fragments another writer rewrote runs again on the latest version. Every row
must come out exactly once, every change must land, pylance must read the result as nanolance
does, and the index must still answer. Data and deletion files are named at random, so writers
in forked processes never take each other's names.
"""

from __future__ import annotations

import multiprocessing as mp
import threading

import pyarrow as pa
import pyarrow.compute as pc

import nanolance.lance as nl
from tests.support import require_pylance

APPENDERS, APPENDS, ROWS = 4, 8, 25


def _append(path, k):
    for i in range(APPENDS):
        nl.write_dataset(pa.table({"id": [k * 1000 + i] * ROWS, "v": [1] * ROWS}), path, mode="append")


def _compact(path, _):
    for _ in range(3):
        nl.dataset(path).optimize.compact_files(target_rows_per_fragment=200)


def _delete(path, _):
    for i in range(5):
        nl.dataset(path).delete(f"id = {i}")


def _update(path, _):
    nl.dataset(path).update({"v": "2"}, where="id = 7")


def _merge(path, _):
    nl.dataset(path).merge_insert("id").when_matched_update_all().when_not_matched_insert_all().execute(
        pa.table({"id": [9, 999999], "v": [3, 3]}))


def _setup(path):
    nl.write_dataset(pa.table({"id": list(range(100)), "v": [1] * 100}), path, max_rows_per_file=20)
    nl.dataset(path).create_scalar_index("id", "BTREE")


def _check(path, others):
    lance = require_pylance()
    theirs = lance.dataset(path).to_table()
    ours = nl.dataset(path).to_table()
    assert theirs.sort_by("id").equals(ours.sort_by("id"))
    ids = theirs.column("id")
    assert pc.sum(pc.greater_equal(ids, 1000)).as_py() - (1 if "m" in others else 0) == APPENDERS * APPENDS * ROWS
    expected_base = 100 - (5 if "d" in others else 0)
    assert pc.sum(pc.less(ids, 100)).as_py() == expected_base
    if "u" in others:
        assert pc.sum(pc.equal(theirs["v"], 2)).as_py() == 1
    if "m" in others:
        assert pc.sum(pc.equal(theirs["v"], 3)).as_py() == 2
    assert lance.dataset(path).to_table(filter="id = 50").num_rows == 1


def test_processes(tmp_path):
    path = str(tmp_path / "p.lance")
    _setup(path)
    ctx = mp.get_context("fork")
    others = "cdum"
    jobs = [ctx.Process(target=_append, args=(path, k)) for k in range(1, APPENDERS + 1)]
    jobs += [ctx.Process(target=f, args=(path, 0)) for f in (_compact, _delete, _update, _merge)]
    for j in jobs:
        j.start()
    for j in jobs:
        j.join()
    assert [j.exitcode for j in jobs] == [0] * len(jobs)
    _check(path, others)


def test_threads(tmp_path):
    path = str(tmp_path / "t.lance")
    _setup(path)
    errors = []

    def run(f, k):
        try:
            f(path, k)
        except Exception as exc:  # noqa: BLE001 -- reported below
            errors.append(exc)

    threads = [threading.Thread(target=run, args=(_append, k)) for k in range(1, APPENDERS + 1)]
    threads += [threading.Thread(target=run, args=(f, 0)) for f in (_compact, _delete)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert errors == []
    _check(path, "cd")
