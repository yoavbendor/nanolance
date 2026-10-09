"""Shapes the writer used to refuse, against pylance: empty structs, null elements inside
fixed-size lists, Arrow dictionary columns, pydantic enums -- and the zero-dimension fixed-size list
both libraries refuse.

Each is written by both libraries and read back by both; the tables must match the input exactly
(for a dictionary column: its type, its dictionary with unused entries, and its values).
"""

from __future__ import annotations

import random

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


def _both_ways(tmp_path, table, name, **write):
    lance = require_pylance()
    for writer in (nl, lance):
        path = str(tmp_path / f"{name}_{writer.__name__}")
        writer.write_dataset(table, path, **write)
        for reader in (nl, lance):
            got = reader.dataset(path).to_table()
            assert got.equals(table), (name, writer.__name__, reader.__name__)
        n = table.num_rows
        rows = sorted({0, n - 1, n // 2})
        assert nl.dataset(path).take(rows).equals(table.take(rows))


def test_empty_struct(tmp_path):
    table = pa.table({"id": [0, 1, 2], "empties": pa.array([{}] * 3, pa.struct([]))})
    _both_ways(tmp_path, table, "top")
    _both_ways(tmp_path, pa.table({"empties": pa.array([{}] * 2, pa.struct([]))}), "alone")
    nested = pa.table({"s": pa.array([{"e": {}, "x": i} for i in range(3)],
                                     pa.struct([("e", pa.struct([])), ("x", pa.int64())]))})
    path = str(tmp_path / "nested")
    nl.write_dataset(nested, path)
    assert nl.dataset(path).to_table().equals(nested)  # (pylance misreads its own file of this shape)


@pytest.mark.parametrize("column", [
    pa.array([{}, None], pa.struct([])),
    pa.array([{"e": {}}, None], pa.struct([("e", pa.struct([]))])),
    pa.array([[{}], []], pa.list_(pa.struct([]))),
])
def test_empty_struct_refused_as_pylance_refuses(tmp_path, column):
    lance = require_pylance()
    for lib in (nl, lance):
        with pytest.raises(OSError, match="Empty structs with rep/def information are not yet supported"):
            lib.write_dataset(pa.table({"c": column}), str(tmp_path / lib.__name__))


@pytest.mark.parametrize("rows,dim,typ,null_row,null_element", [
    (3, 2, pa.float32(), 0.3, 0.3),
    (5000, 3, pa.int64(), 0.1, 0.2),
    (3000, 128, pa.float32(), 0.0, 0.01),
    (100, 1000, pa.float64(), 0.2, 0.05),
])
def test_fixed_size_list_null_elements(tmp_path, rows, dim, typ, null_row, null_element):
    rng = random.Random(rows)
    values = [None if rng.random() < null_row else
              [None if rng.random() < null_element else rng.randint(-100, 100) for _ in range(dim)]
              for _ in range(rows)]
    table = pa.table({"id": range(rows), "v": pa.array(values, pa.list_(typ, dim))})
    _both_ways(tmp_path, table, "fsl", max_rows_per_file=1500)


def test_fixed_size_list_null_elements_from_a_later_batch(tmp_path):
    lance = require_pylance()
    first = pa.array([[1.0, 2.0]] * 5, pa.list_(pa.float32(), 2))
    later = pa.array([[None, 2.0], [3.0, None]], pa.list_(pa.float32(), 2))
    path = str(tmp_path / "later")
    nl.write_dataset(pa.table({"v": first}), path)
    nl.write_dataset(pa.table({"v": later}), path, mode="append")
    expected = first.to_pylist() + later.to_pylist()
    assert lance.dataset(path).to_table()["v"].to_pylist() == expected
    nl.dataset(path).optimize.compact_files()
    assert lance.dataset(path).to_table()["v"].to_pylist() == expected
    assert nl.dataset(path).to_table()["v"].to_pylist() == expected


DICTIONARIES = {
    "string_int32": pa.array(["a", "b", "a", None]).dictionary_encode(),
    "int64_int8": pa.DictionaryArray.from_arrays(pa.array([0, 1, 0], pa.int8()), pa.array([10, 20])),
    "unused_entries": pa.DictionaryArray.from_arrays(pa.array([0, 0, 1, 1], pa.int32()),
                                                     pa.array(["foo", "bar", "baz"])),
    "uint16_many": pa.DictionaryArray.from_arrays(pa.array([i % 300 for i in range(5000)], pa.uint16()),
                                                  pa.array([f"v{i}" for i in range(300)])),
    "uint64": pa.DictionaryArray.from_arrays(pa.array([2, 1, 0, None], pa.uint64()), pa.array(["x", "y", "z"])),
    "int64": pa.DictionaryArray.from_arrays(pa.array([2, 1, 0, None], pa.int64()), pa.array(["x", "y", "z"])),
    "large_string": pa.DictionaryArray.from_arrays(pa.array([0, 1, 1], pa.int16()),
                                                   pa.array(["p", "q"], pa.large_string())),
    "double": pa.DictionaryArray.from_arrays(pa.array([0, 1, 1, 0], pa.int32()), pa.array([1.5, 2.5])),
    "uint8": pa.DictionaryArray.from_arrays(pa.array([0, 1, 2, 0, 1, 2], pa.uint8()),
                                            pa.array(["foo", "bar", "baz"])),
}


@pytest.mark.parametrize("name", sorted(DICTIONARIES))
def test_dictionary_columns(tmp_path, name):
    column = DICTIONARIES[name]
    table = pa.table({"id": range(len(column)), "d": column})
    _both_ways(tmp_path, table, name)
    for lib in (nl, require_pylance()):
        path = str(tmp_path / f"{name}_{lib.__name__}")
        ds = nl.dataset(path)
        assert ds.schema.field("d").type == column.type
        got = ds.to_table()["d"].chunk(0)
        assert got.dictionary.equals(column.dictionary)
        expected = table.filter(pa.compute.greater(table["id"], 0))
        assert ds.to_table(filter="id > 0")["d"].to_pylist() == expected["d"].to_pylist()


def test_dictionary_appends_and_merge_insert(tmp_path):
    """pylance's test_dictionaries, compared with pylance itself."""
    lance = require_pylance()
    dtype = pa.dictionary(pa.int32(), pa.string())
    batches = [pa.table({"id": [1, 2, 3], "dict": pa.array(["foo", "bar", "baz"], dtype)}),
               pa.table({"id": [4, 5, 6], "dict": pa.array(["qux", "quux", "corge"], dtype)})]
    upsert = pa.table({"id": [1, 7], "dict": pa.array(["grault", "garply"], dtype)})
    out = []
    for lib in (nl, lance):
        path = str(tmp_path / lib.__name__)
        ds = lib.write_dataset(batches[0], path)
        ds.insert(batches[1])
        combined = lib.dataset(path).to_table().combine_chunks()
        lib.dataset(path).merge_insert("id").when_matched_update_all().when_not_matched_insert_all().execute(upsert)
        final = lib.dataset(path).to_table().combine_chunks().sort_by("id")
        out.append((combined, final["dict"].to_pylist(), lib.dataset(path).schema))
    assert out[0][0].equals(out[1][0])
    assert out[0][0]["dict"].chunk(0).dictionary.to_pylist() == ["foo", "bar", "baz", "qux", "quux", "corge"]
    assert out[0][1] == out[1][1]
    assert out[0][2] == out[1][2]


def test_dictionary_file_round_trip(tmp_path):
    from nanolance.lance.file import LanceFileReader, LanceFileWriter

    dictionary = pa.array(["foo", "bar", "baz"])
    for index_type in (pa.uint8(), pa.uint16(), pa.uint32(), pa.uint64(), pa.int8(), pa.int16(), pa.int32(),
                       pa.int64()):
        column = pa.DictionaryArray.from_arrays(pa.array([0, 0, 1, 1], index_type), dictionary)
        path = str(tmp_path / f"{index_type}.lance")
        with LanceFileWriter(path) as writer:
            writer.write_batch(pa.table({"dict": column}))
        got = LanceFileReader(path).read_all().to_table()["dict"].chunk(0)
        assert got.equals(column) and got.dictionary.equals(dictionary)


def test_dictionary_refusals(tmp_path):
    nested = pa.table({"s": pa.StructArray.from_arrays([pa.array(["a"]).dictionary_encode()], ["d"])})
    with pytest.raises(NotImplementedError, match="inside a struct or list is not supported yet"):
        nl.write_dataset(nested, str(tmp_path / "nested"))


def test_zero_dimension_fixed_size_list(tmp_path):
    lance = require_pylance()
    table = pa.table({"vec": pa.array([[]], pa.list_(pa.float32(), 0))})
    for lib in (nl, lance):
        with pytest.raises(OSError, match="dimension must be a positive integer"):
            lib.write_dataset(table, str(tmp_path / lib.__name__))


def test_pydantic_enums(tmp_path):
    pytest.importorskip("pydantic")
    from enum import Enum

    from pydantic import BaseModel

    from nanolance.lance.pydantic import pydantic_to_schema

    class Color(str, Enum):
        RED = "red"
        GREEN = "green"

    class Priority(int, Enum):
        LOW = 0
        HIGH = 1

    class Item(BaseModel):
        color: Color
        priority: Priority

    schema = pydantic_to_schema(Item)
    assert schema.field("color").type == pa.dictionary(pa.int32(), pa.utf8())
    assert schema.field("priority").type == pa.int64()
    ds = nl.LanceDataset.from_pydantic_model(Item, [Item(color=Color.RED, priority=Priority.HIGH),
                                                     Item(color=Color.GREEN, priority=Priority.LOW)],
                                             str(tmp_path / "items"))
    assert ds.to_table()["color"].to_pylist() == ["red", "green"]
