"""Streaming / chunked LanceWriter: context manager, write_batch, flush, close."""

from __future__ import annotations

import glob
import os

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


def _make_batch(schema, chunk_id, rows):
    lo = chunk_id * rows
    return pa.record_batch(
        {
            "id": pa.array(range(lo, lo + rows), type=pa.int64()),
            "value": pa.array([float(i) for i in range(rows)], type=pa.float64()),
        },
        schema=schema,
    )


def _fragment_files(path):
    return glob.glob(os.path.join(str(path), "data", "*.lance"))


SCHEMA = pa.schema([("id", pa.int64()), ("value", pa.float64())])


def test_chunked_write_single_fragment_roundtrip(tmp_path):
    path = tmp_path / "single.lance"
    n_chunks, rows = 20, 1_000
    with nanolance.LanceWriter(path) as w:
        for chunk_id in range(n_chunks):
            batch = _make_batch(SCHEMA, chunk_id, rows)
            w.write_batch(batch)
            del batch  # only one chunk in Python memory at a time

    # Default: everything committed as a single fragment.
    assert len(_fragment_files(path)) == 1

    table = pa.table(nanolance.read_table(path))
    assert table.num_rows == n_chunks * rows
    assert table.column("id").to_pylist() == list(range(n_chunks * rows))


def test_max_rows_per_fragment_bounds_and_roundtrips(tmp_path):
    path = tmp_path / "multi.lance"
    n_chunks, rows = 10, 5_000  # 50k rows total
    with nanolance.LanceWriter(path, max_rows_per_fragment=10_000) as w:
        for chunk_id in range(n_chunks):
            w.write_batch(_make_batch(SCHEMA, chunk_id, rows))

    # 50k rows / 10k-per-fragment cap -> 5 fragments (each 2 batches).
    frags = _fragment_files(path)
    assert len(frags) == 5, f"expected 5 fragments, got {len(frags)}"

    table = pa.table(nanolance.read_table(path))
    assert table.num_rows == n_chunks * rows
    assert table.column("id").to_pylist() == list(range(n_chunks * rows))


def test_read_is_chunked_per_fragment(tmp_path):
    path = tmp_path / "chunked_read.lance"
    with nanolance.LanceWriter(path, max_rows_per_fragment=1_000) as w:
        for chunk_id in range(4):
            w.write_batch(_make_batch(SCHEMA, chunk_id, 1_000))

    # The read handle exports an Arrow C stream that yields one batch per fragment,
    # so a consumer can process it chunk by chunk without materializing one array.
    reader = pa.RecordBatchReader.from_stream(nanolance.read_table(path))
    batches = list(reader)
    assert len(batches) == 4
    assert sum(b.num_rows for b in batches) == 4_000


def test_flush_forces_fragment_boundary(tmp_path):
    path = tmp_path / "flush.lance"
    with nanolance.LanceWriter(path) as w:
        w.write_batch(_make_batch(SCHEMA, 0, 1_000))
        w.flush()  # fragment 1
        w.write_batch(_make_batch(SCHEMA, 1, 1_000))
        # fragment 2 committed on close
    assert len(_fragment_files(path)) == 2
    table = pa.table(nanolance.read_table(path))
    assert table.num_rows == 2_000


def test_explicit_close_then_read(tmp_path):
    path = tmp_path / "explicit.lance"
    w = nanolance.LanceWriter(path)
    w.write_batch(_make_batch(SCHEMA, 0, 500))
    w.close()
    with pytest.raises(RuntimeError):
        w.write_batch(_make_batch(SCHEMA, 1, 500))  # closed
    assert pa.table(nanolance.read_table(path)).num_rows == 500


def test_empty_writer_raises_on_close(tmp_path):
    path = tmp_path / "empty.lance"
    with pytest.raises(RuntimeError):
        with nanolance.LanceWriter(path):
            pass  # no batches written


def test_exception_in_block_does_not_mask(tmp_path):
    path = tmp_path / "exc.lance"

    class Boom(Exception):
        pass

    with pytest.raises(Boom):
        with nanolance.LanceWriter(path) as w:
            w.write_batch(_make_batch(SCHEMA, 0, 100))
            raise Boom()


def test_compression_and_structural_options(tmp_path):
    # zstd on a high-cardinality-ish column via the streaming writer.
    path = tmp_path / "zstd.lance"
    schema = pa.schema([("s", pa.string())])
    with nanolance.LanceWriter(path, compression=True) as w:
        for chunk_id in range(5):
            vals = [f"obj-{chunk_id}-{i:08x}" for i in range(2_000)]
            w.write_batch(pa.record_batch({"s": pa.array(vals, type=pa.string())}, schema=schema))
    table = pa.table(nanolance.read_table(path))
    assert table.num_rows == 10_000


def test_matches_write_table_output(tmp_path):
    # A single-fragment streaming write should round-trip to the same values as write_table.
    stream_path = tmp_path / "stream.lance"
    bulk_path = tmp_path / "bulk.lance"
    batches = [_make_batch(SCHEMA, c, 1_000) for c in range(5)]

    with nanolance.LanceWriter(stream_path) as w:
        for b in batches:
            w.write_batch(b)
    nanolance.write_table(pa.Table.from_batches(batches, schema=SCHEMA), bulk_path)

    assert (
        pa.table(nanolance.read_table(stream_path)).column("id").to_pylist()
        == pa.table(nanolance.read_table(bulk_path)).column("id").to_pylist()
    )


def test_sliced_batches_of_a_string_column_roundtrip(tmp_path):
    """A batch that is a SLICE of a bigger array must contribute its own rows, not the first batch's.

    `Table.to_batches()` -- the obvious way to feed this writer, and what every example here does --
    does not copy: each batch shares one contiguous buffer and addresses its rows through
    `array.offset`. The variable-width ingest path ignored that offset (the fixed-width path always
    applied it), so from row `max_chunksize` on, every utf8/binary column silently re-ingested the
    FIRST batch's offsets and data. Silent corruption, and stock Lance read back the same wrong
    values -- the bytes on disk were wrong, not the reader.

    Fixed-width columns are written alongside as the control: they were correct throughout, so a
    failure here is specific to the string/binary path.
    """
    n, chunk = 30_000, 2_500
    table = pa.table(
        {
            "id": pa.array(range(n), type=pa.int64()),
            "s": pa.array([f"v{i}" for i in range(n)], type=pa.utf8()),
            "b": pa.array([f"v{i}".encode() for i in range(n)], type=pa.binary()),
            "nullable": pa.array(
                [None if i % 7 == 0 else f"v{i}" for i in range(n)], type=pa.utf8()
            ),
        }
    )
    path = tmp_path / "sliced.lance"
    with nanolance.LanceWriter(path, max_rows_per_fragment=10_000) as w:
        for batch in table.to_batches(max_chunksize=chunk):
            assert batch.column(1).offset or batch.num_rows == chunk  # slices, not copies
            w.write_batch(batch)

    assert pa.table(nanolance.read_table(path)).to_pydict() == table.to_pydict()


def test_a_sliced_first_batch_is_not_rebased_to_row_zero(tmp_path):
    """The offset also has to be honoured on the FIRST batch, which takes a separate fast path."""
    source = pa.array([f"v{i}" for i in range(1_000)], type=pa.utf8())
    sliced = source.slice(400, 100)
    path = tmp_path / "sliced_first.lance"
    with nanolance.LanceWriter(path) as w:
        w.write_batch(pa.record_batch({"s": sliced}))

    assert pa.table(nanolance.read_table(path)).column("s").to_pylist() == sliced.to_pylist()


def test_empty_batches_between_populated_ones(tmp_path):
    """A zero-length batch must neither be skipped wrongly nor shift the offsets that follow."""
    path = tmp_path / "empty_mixed.lance"
    with nanolance.LanceWriter(path) as w:
        w.write_batch(pa.record_batch({"s": pa.array(["x", "y"], type=pa.utf8())}))
        w.write_batch(pa.record_batch({"s": pa.array([], type=pa.utf8())}))
        w.write_batch(pa.record_batch({"s": pa.array(["z"], type=pa.utf8())}))

    assert pa.table(nanolance.read_table(path)).column("s").to_pylist() == ["x", "y", "z"]


def test_stock_lance_reads_sliced_batches_correctly(tmp_path):
    """The cross-check that identified the sliced-batch bug as a WRITER bug, kept as a guard.

    When the variable-width ingest path ignored `array.offset`, nanolance and pylance read back the
    same wrong values -- agreement between the two readers is what proved the bytes on disk were
    wrong. So the guard has to be here, on stock Lance, and not only on our own round trip.
    """
    lance = require_pylance()
    n = 30_000
    table = pa.table(
        {
            "s": pa.array([f"v{i}" for i in range(n)], type=pa.utf8()),
            "nullable": pa.array(
                [None if i % 7 == 0 else f"v{i}" for i in range(n)], type=pa.utf8()
            ),
        }
    )
    path = tmp_path / "sliced_parity.lance"
    with nanolance.LanceWriter(path, max_rows_per_fragment=10_000) as writer:
        for batch in table.to_batches(max_chunksize=2_500):
            writer.write_batch(batch)

    assert lance.dataset(str(path)).to_table().to_pydict() == table.to_pydict()
