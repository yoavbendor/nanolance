"""The writer's memory budget: ``max_pending_bytes`` (C: nano_lance_writer_set_max_pending_bytes).

nanolance buffers every batch until a commit. For a device that must keep its resident set small
while saving, ``max_pending_bytes`` makes the writer commit a fragment itself whenever the data it
holds reaches the budget. What has to hold, beyond "the memory goes down" (measured in
tests/test_writer_memory.cpp, which can see the process's RSS without Python's own allocations in
the way):

  * nothing is lost or reordered across the flushes, for every page layout that commits a
    fragment -- flat, dictionary, FSST, lists -- and both readers agree;
  * the flushes are ordinary fragments: row ranges across them, deletions, and appending to the
    dataset afterwards all work;
  * the caller's code does not change: close() (or a final commit) after automatic flushes;
  * the knob is reachable from the CLI.
"""

from __future__ import annotations

import random

import pyarrow as pa
import pytest

import nanolance
from nanolance.__main__ import main
from tests.support import require_pylance

N = 60_000
BATCH = 4_096


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _table(n=N, seed=1):
    rng = random.Random(seed)
    return pa.table(
        {
            "id": pa.array(range(n), pa.int64()),
            "small": pa.array([i % 100 for i in range(n)], pa.int32()),
            "tag": pa.array([f"t{i % 7}" for i in range(n)]),
            "text": pa.array([f"user-{rng.getrandbits(32):08x}@mail{i % 97}.example.org" for i in range(n)]),
            "maybe": pa.array([None if i % 13 == 0 else float(i) for i in range(n)]),
            "items": pa.array([[f"i{i}-{j}" for j in range(i % 4)] for i in range(n)], pa.list_(pa.utf8())),
        }
    )


def _write(path, table, **kwargs):
    with nanolance.LanceWriter(path, **kwargs) as writer:
        for batch in table.to_batches(max_chunksize=BATCH):
            writer.write_batch(batch)


@pytest.mark.parametrize("budget", [1, 256 << 10, 1 << 20])
def test_a_budget_splits_the_write_into_fragments_and_loses_nothing(lance_mod, tmp_path, budget):
    """1 byte flushes every batch; 256 KiB and 1 MiB a few batches at a time."""
    table = _table()
    path = tmp_path / "budget.lance"
    _write(path, table, max_pending_bytes=budget)
    fragments = len(lance_mod.dataset(str(path)).get_fragments())
    assert fragments > 1, "the budget never flushed"
    if budget == 1:
        assert fragments == -(-N // BATCH), "a 1-byte budget flushes every batch"
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == table.to_pydict()
    assert lance_mod.dataset(str(path)).to_table().to_pydict() == table.to_pydict()
    expected = table.slice(BATCH * 3 - 100, 250).to_pydict()  # across a flush boundary
    assert pa.table(nanolance.read_table(path, offset=BATCH * 3 - 100, length=250)).to_pydict() == expected


def test_budget_and_row_limit_together_and_a_later_append(lance_mod, tmp_path):
    """Whichever limit is reached first flushes. The flushed dataset is an ordinary one: append to
    it with another budgeted writer, then delete from it."""
    table = _table()
    path = tmp_path / "both.lance"
    _write(path, table, max_pending_bytes=512 << 10, max_rows_per_fragment=10_000)
    more = _table(5_000, seed=2)
    more = more.set_column(0, "id", pa.array(range(N, N + 5_000), pa.int64()))
    _write(path, more, max_pending_bytes=64 << 10, append=True)
    everything = pa.concat_tables([table, more])
    assert pa.table(nanolance.read_table(path)).to_pydict() == everything.to_pydict()
    lance_mod.dataset(str(path)).delete("id % 3 == 0")
    assert (
        pa.table(nanolance.read_table(path)).to_pydict() == lance_mod.dataset(str(path)).to_table().to_pydict()
    )


def test_an_explicit_flush_after_an_automatic_one(tmp_path):
    """flush() in between and close() at the end, with the writer also flushing on its own."""
    table = _table(20_000)
    path = tmp_path / "flush.lance"
    with nanolance.LanceWriter(path, max_pending_bytes=200 << 10) as writer:
        for k, batch in enumerate(table.to_batches(max_chunksize=1_000)):
            writer.write_batch(batch)
            if k % 7 == 3:
                writer.flush()
    assert pa.table(nanolance.read_table(path)).to_pydict() == table.to_pydict()


def test_a_negative_budget_is_refused(tmp_path):
    with pytest.raises(ValueError):
        nanolance.LanceWriter(tmp_path / "x.lance", max_pending_bytes=-1)


def test_convert_takes_a_byte_budget(tmp_path, lance_mod):
    import pyarrow.parquet as pq

    table = _table()
    source = tmp_path / "in.parquet"
    pq.write_table(table, source, row_group_size=BATCH)
    dest = tmp_path / "out.lance"
    assert (
        main(["convert", str(source), str(dest), "--rows-per-fragment", "0", "--batch-size", str(BATCH),
              "--max-pending-bytes", str(256 << 10)])
        == 0
    )
    assert len(lance_mod.dataset(str(dest)).get_fragments()) > 1
    assert pa.table(nanolance.read_table(dest)).to_pydict() == table.to_pydict()
