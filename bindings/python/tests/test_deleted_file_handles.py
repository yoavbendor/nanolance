# SPDX-License-Identifier: Apache-2.0
"""Files nanolance deletes are not held open afterwards. The reader keeps a few data files open per thread;
cleanup (and dropping a dataset, removing an index) now closes those handles in every thread, so a deleted
file's space is freed at once instead of when each thread happens to read other files (on NFS the file had
lingered as .nfsXXXX). Linux only: /proc/self/fd says which open files were deleted."""

import os
import sys
import threading
from datetime import timedelta

import pyarrow as pa
import pytest

import nanolance.lance as nl

pytestmark = pytest.mark.skipif(not sys.platform.startswith("linux"), reason="reads /proc/self/fd")


def _deleted_open(prefix: str) -> list:
    out = []
    for fd in os.listdir("/proc/self/fd"):
        try:
            target = os.readlink(f"/proc/self/fd/{fd}")
        except OSError:
            continue
        if target.endswith(" (deleted)") and target.startswith(prefix):
            out.append(target)
    return out


def _history(path: str) -> None:
    t = lambda base: pa.table({"id": pa.array(range(base, base + 2000), pa.int64()), "v": pa.array([float(i) for i in range(2000)])})
    nl.write_dataset(t(0), path, max_rows_per_file=500)
    for k in range(1, 4):
        nl.write_dataset(t(k * 2000), path, mode="append", max_rows_per_file=500)
        nl.dataset(path).to_table()  # read every version's files, on the reader's worker threads
    nl.dataset(path).delete("id < 3000")
    nl.dataset(path).optimize.compact_files()
    nl.dataset(path).to_table()


def test_cleanup_closes_handles_to_the_files_it_deletes(tmp_path):
    path = str(tmp_path / "ds.lance")
    _history(path)
    # Read old versions too, so their (soon unreferenced) files are cached open.
    for v in range(1, nl.dataset(path).version):
        nl.dataset(path, version=v).to_table()
    stats = nl.dataset(path).cleanup_old_versions(older_than=timedelta(0))
    assert stats.data_files_removed > 0
    assert _deleted_open(path) == []


def test_drop_closes_handles(tmp_path):
    path = str(tmp_path / "ds.lance")
    _history(path)
    nl.dataset(path).to_table()
    nl.LanceDataset.drop(path)
    assert _deleted_open(path) == []


def test_cleanup_while_other_threads_read(tmp_path):
    """Cleanup closes handles in threads that are reading at the same time: no crash and no deadlock. A
    reader whose version cleanup removes mid-read gets "not found"; anything else is a failure."""
    busy = str(tmp_path / "busy.lance")
    other = str(tmp_path / "other.lance")
    _history(busy)
    _history(other)
    stop = threading.Event()
    errors = []

    def reader(p):
        try:
            while not stop.is_set():
                assert nl.dataset(p).to_table().num_rows > 0
        except Exception as e:  # noqa: BLE001 - reported below
            # A reader that opened a version cleanup then removed (older_than=0) is told so, as pylance's is.
            if "not found" not in str(e) and not isinstance(e, FileNotFoundError):
                errors.append(e)

    threads = [threading.Thread(target=reader, args=(p,)) for p in (busy, busy, other)]
    for t in threads:
        t.start()
    try:
        for _ in range(3):
            nl.write_dataset(pa.table({"id": pa.array([1], pa.int64()), "v": pa.array([1.0])}), busy, mode="append")
            nl.dataset(busy).optimize.compact_files()
            nl.dataset(busy).cleanup_old_versions(older_than=timedelta(0))
    finally:
        stop.set()
        for t in threads:
            t.join(timeout=60)
    assert not any(t.is_alive() for t in threads), "a reader did not finish"
    assert errors == []
