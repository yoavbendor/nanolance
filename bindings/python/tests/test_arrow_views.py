"""Utf8View and BinaryView columns: written as string and binary, as pylance writes them.

pylance 12.0.0 turns a ``string_view`` / ``binary_view`` column into a ``string`` / ``binary`` column on
write, and its data files are byte-identical to the ones it writes for the same values as plain strings.
nanolance converts the view columns when it writes (src/arrow/arrow_views.cpp), so for each case here:

1. the schema nanolance writes is the schema pylance writes (the view types are not kept);
2. both readers read back the values that were written, nulls included;
3. a view column's data files are the ones nanolance writes for the same values as plain strings.

The cases cover what the conversion has to get right: inline and out-of-line values, several data buffers,
a sliced array (offset != 0), nulls, empty and all-null columns, and views inside a struct and a list.
"""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path

import pyarrow as pa
import pytest

import nanolance
import nanolance.lance
from tests.support import require_pylance

SV = pa.string_view()
BV = pa.binary_view()
LONG = ["x" * 30, None, "short", "y" * 13, "", "z" * 40, "exactly12chr", "w" * 14]


def _multi_buffer_view():
    """A string_view whose values sit in two data buffers (pyarrow builds one; this builds the layout)."""
    d1 = b"hello world, this is buffer one"
    d2 = b"second buffer holds this longer string"

    def view(value, buffer, offset):
        if len(value) <= 12:
            return struct.pack("<i", len(value)) + value.ljust(12, b"\0")
        return struct.pack("<i", len(value)) + value[:4] + struct.pack("<ii", buffer, offset)

    views = view(d1, 0, 0) + view(d2, 1, 0) + view(b"abc", 0, 0) + view(d2[:20], 1, len(d2))
    data2 = d2 + d2[:20]
    buffers = [None, pa.py_buffer(views), pa.py_buffer(d1), pa.py_buffer(data2),
               pa.py_buffer(struct.pack("<qq", len(d1), len(data2)))]
    return pa.Array.from_buffers(SV, 4, buffers)


_SLICED = pa.array(["aaaaaaaaaaaaaaaaaaaaaa", "bb", None, "cccccccccccccccccccccc", "d"], SV).slice(1, 3)

CASES = {
    "string_view_nulls_inline_and_long": pa.table({"s": pa.array(LONG, SV)}),
    "binary_view_with_zero_bytes": pa.table({"b": pa.array([b"x", b"", None, b"\x00\xff" * 10], BV)}),
    "all_null_string_view": pa.table({"s": pa.array([None, None, None], SV)}),
    "all_null_binary_view": pa.table({"b": pa.array([None, None], BV)}),
    "sliced_string_view": pa.table({"s": _SLICED}),
    "several_data_buffers": pa.table({"s": _multi_buffer_view()}),
    "struct_of_string_view": pa.table({"st": pa.StructArray.from_arrays(
        [pa.array(["a", None, "a long value over 12"], SV)], names=["x"])}),
    "list_of_string_view": pa.table({"l": pa.ListArray.from_arrays(
        pa.array([0, 2, 2, 3], pa.int32()), pa.array(["p", "a longer value here", None], SV))}),
    "several_columns": pa.table({
        "id": pa.array([1, 2, 3, 4], pa.int64()),
        "s": pa.array(LONG[:4], SV),
        "b": pa.array([b"q" * 20, None, b"r", b""], BV),
    }),
}


def _plain_type(arrow_type: pa.DataType) -> pa.DataType:
    """The type with every view type, at any depth, as the string / binary type it is written as."""
    if arrow_type == SV:
        return pa.string()
    if arrow_type == BV:
        return pa.binary()
    if pa.types.is_struct(arrow_type):
        return pa.struct([pa.field(f.name, _plain_type(f.type), f.nullable) for f in arrow_type])
    if pa.types.is_list(arrow_type):
        return pa.list_(_plain_type(arrow_type.value_type))
    return arrow_type


def _plain(table: pa.Table) -> pa.Table:
    """The same values with the view columns (at any depth) as string / binary."""
    return pa.table({name: table.column(name).cast(_plain_type(table.column(name).type))
                     for name in table.column_names})


def _data_hashes(dataset_path: Path):
    return sorted(hashlib.sha256(p.read_bytes()).hexdigest() for p in (dataset_path / "data").glob("*.lance"))


@pytest.mark.parametrize("name", sorted(CASES))
def test_nanolance_writes_the_schema_pylance_writes(tmp_path, name):
    lance_mod = require_pylance()
    table = CASES[name]
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nanolance.write_table(table, ours)
    lance_mod.write_dataset(table, str(theirs))
    assert str(lance_mod.dataset(str(ours)).schema) == str(lance_mod.dataset(str(theirs)).schema)
    assert "view" not in str(lance_mod.dataset(str(ours)).schema)


@pytest.mark.parametrize("name", sorted(CASES))
def test_both_readers_read_what_nanolance_wrote(tmp_path, name):
    lance_mod = require_pylance()
    table = CASES[name]
    path = tmp_path / "ours.lance"
    nanolance.write_table(table, path)
    expected = _plain(table).to_pydict()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected
    assert lance_mod.dataset(str(path)).to_table().to_pydict() == expected


@pytest.mark.parametrize("name", sorted(CASES))
def test_view_data_is_the_plain_string_data(tmp_path, name):
    """A view column is stored as its plain string would be: the same data files, byte for byte."""
    table = CASES[name]
    view_path, plain_path = tmp_path / "view.lance", tmp_path / "plain.lance"
    nanolance.write_table(table, view_path)
    nanolance.write_table(_plain(table), plain_path)
    assert _data_hashes(view_path) == _data_hashes(plain_path)


@pytest.mark.parametrize("name", sorted(CASES))
def test_pylance_reads_nanolance_view_writes_as_its_own(tmp_path, name):
    """A file nanolance wrote from views reads back, in pylance, the same as pylance's own view write."""
    lance_mod = require_pylance()
    table = CASES[name]
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nanolance.write_table(table, ours)
    lance_mod.write_dataset(table, str(theirs))
    assert lance_mod.dataset(str(ours)).to_table().to_pydict() == lance_mod.dataset(str(theirs)).to_table().to_pydict()


def test_pylance_written_view_is_read_by_nanolance(tmp_path):
    lance_mod = require_pylance()
    table = CASES["several_columns"]
    path = tmp_path / "theirs.lance"
    lance_mod.write_dataset(table, str(path))
    assert pa.table(nanolance.read_table(path)).to_pydict() == _plain(table).to_pydict()


def test_append_of_view_batches_into_a_string_dataset(tmp_path):
    """Appending a view batch to a string dataset writes the same fragments as appending the plain batch."""
    lance_mod = require_pylance()
    base = pa.table({"id": pa.array([1, 2], pa.int64()), "s": pa.array(["one", "two long value over 12"], pa.string())})
    extra = pa.table({"id": pa.array([3, 4], pa.int64()), "s": pa.array(["three long value over 12", None], SV)})
    path = tmp_path / "appended.lance"
    nanolance.write_table(base, path)
    nanolance.write_table(extra, path, append=True)
    expected = pa.concat_tables([base, _plain(extra)]).to_pydict()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected
    assert lance_mod.dataset(str(path)).to_table().to_pydict() == expected
    assert lance_mod.dataset(str(path)).schema.field("s").type == pa.string()


def test_view_batches_in_one_writer_session(tmp_path):
    """Two view batches in one writer session: the second must match the first's schema, as with plain strings."""
    batches = [pa.record_batch({"s": pa.array(["a long string value here", None], SV)}),
               pa.record_batch({"s": pa.array(["b", "a long string value again"], SV)})]
    reader = pa.RecordBatchReader.from_batches(batches[0].schema, batches)
    path = tmp_path / "session.lance"
    nanolance.write_table(reader.read_all(), path)
    expected = {"s": ["a long string value here", None, "b", "a long string value again"]}
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected


def test_empty_view_column(tmp_path):
    """An empty string_view column is written as an empty string column (through write_dataset: write_table
    refuses a table with no rows for every column type)."""
    lance_mod = require_pylance()
    table = pa.table({"s": pa.array([], SV)})
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nanolance.lance.write_dataset(table, str(ours))
    lance_mod.write_dataset(table, str(theirs))
    for path in (ours, theirs):
        read = lance_mod.dataset(str(path)).to_table()
        assert read.schema.field("s").type == pa.string()
        assert read.num_rows == 0
    assert nanolance.lance.dataset(str(ours)).to_table().schema.field("s").type == pa.string()
