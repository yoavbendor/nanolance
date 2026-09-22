"""`read_schema` and `count_rows` -- the cheap questions, answered from the manifest."""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance


@pytest.fixture
def dataset(tmp_path):
    n, frag = 30_000, 10_000
    table = pa.table(
        {
            "id": pa.array(range(n), type=pa.int64()),
            "s": pa.array([None if i % 7 == 0 else f"v{i}" for i in range(n)], type=pa.string()),
            "v": pa.array([i * 0.5 for i in range(n)], type=pa.float64()),
        }
    )
    path = tmp_path / "meta.lance"
    with nanolance.LanceWriter(path, max_rows_per_fragment=frag) as writer:
        for batch in table.to_batches(max_chunksize=frag):
            writer.write_batch(batch)
    return path, table


def test_count_rows_agrees_with_a_full_read(dataset):
    """The point of a cheap count is that it is the SAME number a read would give."""
    path, table = dataset
    assert nanolance.count_rows(path) == table.num_rows
    assert nanolance.count_rows(path) == pa.table(nanolance.read_table(path)).num_rows


def test_read_schema_matches_the_schema_a_read_returns(dataset):
    path, table = dataset
    peeked = pa.schema(nanolance.read_schema(path))
    assert peeked.equals(pa.table(nanolance.read_table(path)).schema)
    assert peeked.names == table.column_names


def test_read_schema_handle_is_re_exportable(dataset):
    """Unlike `open_stream`, a schema handle is not single-shot -- a schema is copyable.

    Getting this wrong is a use-after-free rather than an error: the first export would hand the
    capsule the handle's only ArrowSchema, and the second would read released memory.
    """
    path, _ = dataset
    handle = nanolance.read_schema(path)
    first = pa.schema(handle)
    second = pa.schema(handle)
    assert first.equals(second)


def test_neither_opens_a_data_file(dataset):
    """Both answer from the manifest, so deleting the data files must not change the answer.

    That is the actual claim -- "cheap" is not testable by timing, but "did not read the data" is.
    """
    path, table = dataset
    expected_rows = nanolance.count_rows(path)
    expected_schema = pa.schema(nanolance.read_schema(path))
    for data_file in (path / "data").glob("*.lance"):
        data_file.unlink()

    assert nanolance.count_rows(path) == expected_rows == table.num_rows
    assert pa.schema(nanolance.read_schema(path)).equals(expected_schema)
    # ...while an actual read of the same dataset now fails, which is what makes the above meaningful.
    with pytest.raises(RuntimeError):
        nanolance.read_table(path)


@pytest.mark.parametrize("call", [nanolance.count_rows, nanolance.read_schema])
def test_missing_dataset_raises(call, tmp_path):
    with pytest.raises(RuntimeError):
        call(tmp_path / "not_here.lance")
