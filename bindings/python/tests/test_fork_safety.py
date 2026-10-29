"""A process forked from one that has read with nanolance's thread pool reads and commits safely.

The parent's pool threads each register their open-file cache. A forked child has only the forking thread,
and the next threads it starts reuse the dead threads' stacks and thread-local storage, so the registry must
not keep pointing at them. It did: a commit in the child (which removes a staged file, and so walks every
thread's cache to close handles to it) locked whatever the new threads had written there, and the child
crashed, aborted in glibc or hung (seen in test_concurrent_writers.py::test_processes). Forking after the
pool has run is what multiprocessing's default "fork" start method and PyTorch DataLoader workers do.
"""

from __future__ import annotations

import multiprocessing as mp
import os

import pyarrow as pa
import pytest

import nanolance.lance as nl

CHILDREN, ROUNDS, TIMEOUT_S = 6, 4, 60


def _child(path: str, k: int) -> None:
    ds = nl.dataset(path)
    assert ds.to_table().num_rows >= 2_000  # a parallel scan: the child's own pool threads open files
    nl.write_dataset(pa.table({"id": [100_000 + k] * 10, "v": ["c"] * 10}), path, mode="append")
    nl.dataset(path).delete(f"id = {100_000 + k}")  # a second commit, from a cache-holding thread


@pytest.mark.skipif(not hasattr(os, "fork"), reason="needs fork")
def test_fork_after_parallel_reads(tmp_path):
    path = str(tmp_path / "f.lance")
    nl.write_dataset(pa.table({"id": list(range(2_000)), "v": [str(i) for i in range(2_000)]}), path,
                     max_rows_per_file=100)
    ctx = mp.get_context("fork")
    for _ in range(ROUNDS):
        assert nl.dataset(path).to_table().num_rows >= 2_000  # the parent's pool threads open files
        jobs = [ctx.Process(target=_child, args=(path, k)) for k in range(CHILDREN)]
        for j in jobs:
            j.start()
        for j in jobs:
            j.join(TIMEOUT_S)
        hung = [i for i, j in enumerate(jobs) if j.is_alive()]
        for j in jobs:
            if j.is_alive():
                j.kill()
                j.join()
        assert hung == [], f"children {hung} did not finish in {TIMEOUT_S} s"
        assert [j.exitcode for j in jobs] == [0] * CHILDREN
    assert nl.dataset(path).count_rows() == 2_000
