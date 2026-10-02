"""Reads and writes on several threads (nanolance.set_threads / NANOLANCE_THREADS).

With more than one thread a fragment is read as row ranges ("morsels") decoded side by side -- one
Arrow batch each -- and a data file's columns are encoded side by side and appended in order. What
has to hold, whatever the thread count: the same rows (full reads, row ranges, after deletions,
take), and byte-for-byte the same files. The morsels are forced tiny (NANOLANCE_MORSEL_KB, read
once per process, hence the child processes) so that every column type is cut mid-page.
"""

from __future__ import annotations

import glob
import hashlib
import json
import os
import subprocess
import sys

import numpy as np
import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _table(n=30_000, seed=0):
    rng = np.random.default_rng(seed)
    lengths = rng.integers(0, 9, n)
    return pa.table({
        "id": pa.array(np.arange(n, dtype=np.int64)),
        "small": pa.array(rng.integers(0, 100, n, dtype=np.int32)),
        "f": pa.array(rng.random(n), mask=rng.random(n) < 0.1),
        "flag": pa.array(rng.random(n) < 0.3),
        "tag": pa.array([f"tag{i % 7}" for i in range(n)]),
        "text": pa.array([None if i % 17 == 0 else f"user-{rng.integers(1 << 30)}@example.org" for i in range(n)]),
        "blob": pa.array([rng.bytes(int(s)) for s in rng.integers(0, 64, n)], pa.binary()),
        "big": pa.array([rng.bytes(5_000) if i % 97 == 0 else b"" for i in range(n)], pa.binary()),
        "vec": pa.FixedSizeListArray.from_arrays(pa.array(rng.random(n * 4, dtype=np.float32)), 4),
        "items": pa.array([list(range(int(k))) if i % 11 else None for i, k in enumerate(lengths)],
                          pa.list_(pa.int64())),
        "nested": pa.array([[[j] * (j % 3) for j in range(int(k) % 4)] for k in lengths],
                           pa.list_(pa.list_(pa.int32()))),
        "point": pa.array([{"x": i, "y": float(i) / 2} if i % 13 else None for i in range(n)]),
        "attrs": pa.array([[("k", i)] for i in range(n)], pa.map_(pa.utf8(), pa.int64())),
    })


_CHILD = """
import sys, json, nanolance, pyarrow as pa
path, spec = sys.argv[1], json.loads(sys.argv[2])
out = {}
out["full"] = pa.table(nanolance.read_table(path)).to_pydict()
out["range"] = pa.table(nanolance.read_table(path, offset=spec["offset"], length=spec["length"])).to_pydict()
out["take"] = pa.table(nanolance.take(path, spec["take"])).to_pydict()
out["batches"] = len(list(pa.RecordBatchReader.from_stream(nanolance.read_table(path))))
sys.stdout.write(repr(out))
"""


def _read_in_child(path, threads, spec):
    env = {**os.environ, "NANOLANCE_THREADS": str(threads), "NANOLANCE_MORSEL_KB": "1"}
    got = subprocess.run([sys.executable, "-c", _CHILD, str(path), json.dumps(spec)], env=env, check=True,
                         capture_output=True, text=True)
    return eval(got.stdout, {"nan": float("nan")})  # repr of plain Python values


@pytest.mark.parametrize("writer", ["nanolance", "rust"])
def test_reads_agree_across_thread_counts(lance_mod, tmp_path, writer):
    table = _table()
    path = tmp_path / "t.lance"
    if writer == "nanolance":
        with nanolance.LanceWriter(path, max_rows_per_fragment=12_000) as w:
            for batch in table.to_batches(max_chunksize=4_000):
                w.write_batch(batch)
    else:
        lance_mod.write_dataset(table, str(path), data_storage_version="2.2", max_rows_per_file=12_000)
    lance_mod.dataset(str(path)).delete("id % 9 = 4 OR (id >= 13000 AND id < 13500)")
    expected = lance_mod.dataset(str(path)).to_table()
    rng = np.random.default_rng(1)
    spec = {"offset": 5_001, "length": 11_111,
            "take": rng.choice(expected.num_rows, 300, replace=False).tolist()}
    want = {
        "full": expected.to_pydict(),
        "range": expected.slice(spec["offset"], spec["length"]).to_pydict(),
        "take": expected.take(pa.array(spec["take"])).to_pydict(),
    }
    batches = {}
    for threads in (1, 3, 8):
        got = _read_in_child(path, threads, spec)
        for key in ("full", "range", "take"):
            assert got[key] == want[key], (threads, key)
        batches[threads] = got["batches"]
    assert batches[1] == 3  # one per fragment
    assert batches[8] > batches[1]  # the morsels really were cut


def test_written_files_do_not_depend_on_the_thread_count(tmp_path):
    table = _table(8_000, seed=2)
    digests = []
    threads = nanolance.get_threads()
    try:
        for n in (1, 4):
            nanolance.set_threads(n)
            path = tmp_path / f"w{n}.lance"
            nanolance.write_table(table, path)
            (data_file,) = glob.glob(f"{path}/data/*.lance")
            digests.append(hashlib.sha256(open(data_file, "rb").read()).hexdigest())
            assert pa.table(nanolance.read_table(path)).to_pydict() == table.to_pydict()
    finally:
        nanolance.set_threads(threads)
    assert digests[0] == digests[1]


def test_thread_count_api():
    threads = nanolance.get_threads()
    try:
        nanolance.set_threads(3)
        assert nanolance.get_threads() == 3
        nanolance.set_threads(0)  # back to the default
        assert nanolance.get_threads() >= 1
        with pytest.raises(ValueError):
            nanolance.set_threads(-1)
    finally:
        nanolance.set_threads(threads)
