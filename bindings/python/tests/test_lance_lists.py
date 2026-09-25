"""List columns written by pylance, read by nanolance (roadmap Phase C2-C4).

Every shape here is checked against pylance's own read of the same dataset -- whole table, row
ranges, and after deletions -- because each of those takes a different path through the decoded
layers (docs/NESTED_COLUMNS.md): a full read uses the offsets as unravelled, a range re-cuts every
layer from the outside in, and a deletion turns kept rows into kept items through every level.

The shapes cross the four things a list layer can say about an entry (valid, null, empty, and a
null item inside a valid list), nesting depth, and the leaf's value encoding.
"""

from __future__ import annotations

import random

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance

N = 3_000


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _rng(seed):
    return random.Random(seed)


def _ints_lists(n, *, null_lists=False, empty_lists=False, null_items=False, seed=1):
    rng = _rng(seed)
    out = []
    for i in range(n):
        if null_lists and i % 7 == 0:
            out.append(None)
        elif empty_lists and i % 5 == 0:
            out.append([])
        else:
            out.append([None if null_items and (i + j) % 4 == 0 else rng.randrange(1 << 40) for j in range(1 + i % 4)])
    return out


def _text(i, j):
    """Mostly short, but every 50th row's items are over 256 bytes: enough to make pages FullZip."""
    return ("L" * 300 if i % 50 == 1 else "") + f"t{i}-{j}"


SHAPES = {
    "int64": lambda: pa.array(_ints_lists(N), pa.list_(pa.int64())),
    "int64_null_lists": lambda: pa.array(_ints_lists(N, null_lists=True), pa.list_(pa.int64())),
    "int64_empty_lists": lambda: pa.array(_ints_lists(N, empty_lists=True), pa.list_(pa.int64())),
    "int64_null_items": lambda: pa.array(_ints_lists(N, null_items=True), pa.list_(pa.int64())),
    "int64_everything": lambda: pa.array(
        _ints_lists(N, null_lists=True, empty_lists=True, null_items=True), pa.list_(pa.int64())
    ),
    "large_list": lambda: pa.array(_ints_lists(N, empty_lists=True), pa.large_list(pa.int64())),
    "int32_small_values": lambda: pa.array([[j % 7 for j in range(i % 5)] for i in range(N)], pa.list_(pa.int32())),
    "float64": lambda: pa.array([[i * 0.5 + j for j in range(i % 3)] for i in range(N)], pa.list_(pa.float64())),
    "bool": lambda: pa.array([None if i % 9 == 0 else [j % 2 == 0 for j in range(i % 4)] for i in range(N)], pa.list_(pa.bool_())),
    "string": lambda: pa.array(
        [None if i % 7 == 0 else [f"s{i}-{j}" for j in range(i % 3)] for i in range(N)], pa.list_(pa.utf8())
    ),
    "string_null_items": lambda: pa.array(
        [[None if j == 1 else f"v{i * 31 + j}" for j in range(i % 4)] for i in range(N)], pa.list_(pa.utf8())
    ),
    "binary": lambda: pa.array([[bytes([i % 251]) * (j + 1) for j in range(i % 3)] for i in range(N)], pa.list_(pa.binary())),
    "timestamp": lambda: pa.array(
        [[1_700_000_000_000_000 + i * 1000 + j for j in range(i % 3)] for i in range(N)], pa.list_(pa.timestamp("us"))
    ),
    "list_of_lists": lambda: pa.array(
        [[[i + k for k in range(j % 3)] for j in range(i % 4)] for i in range(N)], pa.list_(pa.list_(pa.int64()))
    ),
    "list_of_lists_nulls": lambda: pa.array(
        [
            None if i % 11 == 0 else [None if j == 2 else [None if k == 0 and i % 2 else i + k for k in range(j % 3)] for j in range(i % 4)]
            for i in range(N)
        ],
        pa.list_(pa.list_(pa.int64())),
    ),
    "all_empty": lambda: pa.array([[] for _ in range(N)], pa.list_(pa.int64())),
    "all_null": lambda: pa.array([None for _ in range(N)], pa.list_(pa.int64())),
    # Low-cardinality items: Lance dictionary-encodes them, inside the list.
    "dictionary_items": lambda: pa.array(
        [[f"cat-{(i + j) % 3}" for j in range(i % 3)] for i in range(N)], pa.list_(pa.utf8())
    ),
    # A constant page: every item the same, so the value is inline and only the levels vary.
    "constant_items": lambda: pa.array([[5] * (i % 3) for i in range(N)], pa.list_(pa.int64())),
    "constant_string_items": lambda: pa.array([["same"] * (i % 3) for i in range(N)], pa.list_(pa.utf8())),
    # Structs mixed with lists (C5). A list of structs has one leaf column per field, each carrying
    # the list and struct layers; they must agree, and they fill ONE list array and ONE struct array.
    "list_of_struct": lambda: pa.array([[{"a": i, "b": f"s{j}"} for j in range(i % 3)] for i in range(N)]),
    "list_of_struct_nulls": lambda: pa.array(
        [
            None if i % 7 == 0 else [None if j == 1 else {"a": None if i % 5 == 0 else i, "b": f"s{i}-{j}"} for j in range(i % 3)]
            for i in range(N)
        ]
    ),
    "struct_of_list": lambda: pa.array([{"a": [i] * (i % 3), "b": i} for i in range(N)]),
    "struct_of_list_nulls": lambda: pa.array([None if i % 4 == 0 else {"a": [i] * (i % 3), "b": i} for i in range(N)]),
    # Maps (C6): stored as a list of (key, value) entry structs, read into Arrow's map type.
    "map": lambda: pa.array([[(f"k{j}", i + j) for j in range(i % 3)] for i in range(N)], pa.map_(pa.utf8(), pa.int64())),
    "map_nulls": lambda: pa.array(
        [None if i % 7 == 0 else [(f"k{j}", None if j == 1 else i + j) for j in range(i % 3)] for i in range(N)],
        pa.map_(pa.utf8(), pa.int64()),
    ),
    "map_of_lists": lambda: pa.array(
        [[(i + j, [f"v{j}"] * j) for j in range(i % 3)] for i in range(N)], pa.map_(pa.int32(), pa.list_(pa.utf8()))
    ),
    # Long lists: one row's items span several miniblock chunks.
    "long_lists": lambda: pa.array([list(range(i % 3000)) for i in range(40)], pa.list_(pa.int64())),
    # FullZip list pages (C8): one item of 256+ bytes turns the whole page into a FullZip, with one
    # control word per LEVEL -- `rep << bits_def | def` -- and a value only behind the visible ones.
    # Empty and null lists, null items, nesting, structs and maps each put different levels there.
    "long_strings": lambda: pa.array(
        [
            None if i % 7 == 0 else [] if i % 5 == 0 else [None if (i + j) % 4 == 0 else _text(i, j) for j in range(i % 4)]
            for i in range(N)
        ],
        pa.list_(pa.utf8()),
    ),
    "long_large_binary": lambda: pa.array(
        [[_text(i, j).encode() for j in range(i % 3)] for i in range(N)], pa.large_list(pa.large_binary())
    ),
    "long_list_of_lists": lambda: pa.array(
        [
            None if i % 11 == 0 else [None if j == 2 else [_text(i, k) for k in range(j % 3)] for j in range(i % 4)]
            for i in range(N)
        ],
        pa.list_(pa.list_(pa.utf8())),
    ),
    "long_list_of_struct": lambda: pa.array(
        [
            None if i % 7 == 0 else [None if j == 1 else {"a": i, "t": None if i % 5 == 0 else _text(i, j)} for j in range(i % 3)]
            for i in range(N)
        ]
    ),
    "long_struct_of_list": lambda: pa.array(
        [None if i % 4 == 0 else {"t": [_text(i, j) for j in range(i % 3)], "b": i} for i in range(N)]
    ),
    "long_map_values": lambda: pa.array(
        [None if i % 9 == 0 else [(f"k{j}", _text(i, j)) for j in range(i % 3)] for i in range(N)],
        pa.map_(pa.utf8(), pa.utf8()),
    ),
}


def _write(lance_mod, tmp_path, name, table, **kwargs):
    path = str(tmp_path / f"{name}.lance")
    lance_mod.write_dataset(table, path, **kwargs)
    return path


@pytest.mark.parametrize("name", sorted(SHAPES))
def test_list_reads_back(lance_mod, tmp_path, name):
    table = pa.table({"c": SHAPES[name]()})
    path = _write(lance_mod, tmp_path, name, table)
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == lance_mod.dataset(path).to_table().to_pydict()
    assert got.schema.field("c").type == table.schema.field("c").type


@pytest.mark.parametrize("name", sorted(SHAPES))
def test_list_row_ranges(lance_mod, tmp_path, name):
    column = SHAPES[name]()
    table = pa.table({"id": pa.array(range(len(column)), pa.int64()), "c": column})
    path = _write(lance_mod, tmp_path, name, table)
    expected = lance_mod.dataset(path).to_table()
    n = len(column)
    for offset, length in ((0, 1), (3, 17), (n // 3, n // 3), (n - 5, 5), (0, n)):
        got = pa.table(nanolance.read_table(path, offset=offset, length=length))
        got.validate(full=True)
        assert got.to_pydict() == expected.slice(offset, length).to_pydict(), (offset, length)


@pytest.mark.parametrize("name", sorted(SHAPES))
def test_list_deletions(lance_mod, tmp_path, name):
    column = SHAPES[name]()
    table = pa.table({"id": pa.array(range(len(column)), pa.int64()), "c": column})
    path = _write(lance_mod, tmp_path, name, table)
    lance_mod.dataset(path).delete("id % 5 == 1 OR id < 2")
    expected = lance_mod.dataset(path).to_table()
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == expected.to_pydict()
    if expected.num_rows > 20:
        got = pa.table(nanolance.read_table(path, offset=7, length=13))
        assert got.to_pydict() == expected.slice(7, 13).to_pydict()


def test_lists_across_fragments_and_pages(lance_mod, tmp_path):
    """Several fragments, and enough rows per fragment for several pages each."""
    n = 60_000
    column = pa.array(_ints_lists(n, null_lists=True, empty_lists=True, null_items=True, seed=7), pa.list_(pa.int64()))
    table = pa.table({"id": pa.array(range(n), pa.int64()), "c": column})
    path = _write(lance_mod, tmp_path, "multi", table, max_rows_per_file=25_000)
    expected = lance_mod.dataset(path).to_table()
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == expected.to_pydict()
    got = pa.table(nanolance.read_table(path, offset=24_990, length=30))
    assert got.to_pydict() == expected.slice(24_990, 30).to_pydict()


def test_full_zip_lists_across_pages(lance_mod, tmp_path):
    """FullZip list pages cut at Lance's 32 MiB page limit: ~25 MB of incompressible 700-byte items
    lands in two pages (26,814 + 3,186 rows with this seed). Each page's control words start at a
    row, so the levels unravel page by page; a range and a deletion cross the page boundary."""
    rng = _rng(5)
    n = 30_000
    column = pa.array(
        [
            None if i % 13 == 0 else [] if i % 11 == 0 else [rng.randbytes(700 + i % 50) if (i + j) % 6 else None for j in range(i % 4)]
            for i in range(n)
        ],
        pa.list_(pa.binary()),
    )
    table = pa.table({"id": pa.array(range(n), pa.int64()), "c": column})
    path = _write(lance_mod, tmp_path, "pages", table)
    expected = lance_mod.dataset(path).to_table()
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == expected.to_pydict()
    assert pa.table(nanolance.read_table(path, offset=26_800, length=30)).to_pydict() == expected.slice(26_800, 30).to_pydict()
    lance_mod.dataset(path).delete("id % 3 == 1")
    expected = lance_mod.dataset(path).to_table()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()


def test_list_projection_and_neighbours(lance_mod, tmp_path):
    """A list column beside flat ones, read alone and together."""
    table = pa.table(
        {
            "a": pa.array(range(N), pa.int64()),
            "l": SHAPES["int64_everything"](),
            "s": pa.array([f"x{i}" for i in range(N)]),
        }
    )
    path = _write(lance_mod, tmp_path, "mixed", table)
    expected = lance_mod.dataset(path).to_table()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()
    assert pa.table(nanolance.read_table(path, columns=["l"])).to_pydict() == expected.select(["l"]).to_pydict()


# Pinned: shapes that still fail, by message. Each fails loudly once it reads.
LIST_GAPS = {
    "list_of_vectors": (
        lambda: pa.array([[[1.0, 2.0]] * (i % 3) for i in range(N)], pa.list_(pa.list_(pa.float32(), 2))),
        None,  # reads since the COCO benchmark needed it (tests/test_fsl_in_lists.py)
    ),
}


@pytest.mark.parametrize("name", sorted(LIST_GAPS))
def test_list_gaps_are_pinned(lance_mod, tmp_path, name):
    build, gap = LIST_GAPS[name]
    path = _write(lance_mod, tmp_path, name, pa.table({"c": build()}))
    expected = lance_mod.dataset(path).to_table()
    if gap is None:
        assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()
        return
    with pytest.raises(Exception) as excinfo:
        pa.table(nanolance.read_table(path))
    assert gap in str(excinfo.value), f"{name} now fails differently: {excinfo.value}"


# A NULL STRUCT, not in any list. Before the nested path this read back as a struct whose fields were
# all null -- {"b": None} where pylance says None -- with no error: the leaf's definition levels say
# "null struct" with a level of their own, and the flat decoder folded every non-zero level into
# "null item". Each shape below takes a different page layout under the struct.
NULL_STRUCTS = {
    "fields": lambda: pa.array([None if i % 4 == 0 else {"b": i, "s": f"x{i}"} for i in range(N)]),
    "null_fields_too": lambda: pa.array([None if i % 4 == 0 else {"b": None if i % 3 == 0 else i} for i in range(N)]),
    "constant_fields": lambda: pa.array(
        [None if i % 4 == 0 else {"k": 7, "s": "same", "z": None} for i in range(N)],
        pa.struct([("k", pa.int64()), ("s", pa.utf8()), ("z", pa.int64())]),
    ),
    "long_strings": lambda: pa.array([None if i % 4 == 0 else {"t": "q" * 300 + str(i)} for i in range(N)]),
    "vectors": lambda: pa.array(
        [None if i % 4 == 0 else {"v": [float(i)] * 4} for i in range(N)], pa.struct([("v", pa.list_(pa.float32(), 4))])
    ),
    "nested_struct": lambda: pa.array(
        [None if i % 5 == 0 else {"inner": None if i % 3 == 0 else {"x": i}} for i in range(N)]
    ),
}


@pytest.mark.parametrize("name", sorted(NULL_STRUCTS))
def test_null_structs_read_as_null(lance_mod, tmp_path, name):
    column = NULL_STRUCTS[name]()
    table = pa.table({"id": pa.array(range(len(column)), pa.int64()), "c": column})
    path = _write(lance_mod, tmp_path, name, table)
    expected = lance_mod.dataset(path).to_table()
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.column("c").null_count == expected.column("c").null_count
    assert got.to_pydict() == expected.to_pydict()
    got = pa.table(nanolance.read_table(path, offset=5, length=40))
    assert got.to_pydict() == expected.slice(5, 40).to_pydict()
    lance_mod.dataset(path).delete("id % 3 == 0")
    expected = lance_mod.dataset(path).to_table()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()

