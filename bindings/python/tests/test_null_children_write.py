"""Null-typed children of lists and structs (W1k5): nanolance writes them, as pylance 12 lays them out.

A null-typed leaf (Arrow's `null`) under a list or a struct has no values: each of its items is null, and the page holds
only the repetition and definition levels of the layers above it, as a ConstantLayout with no value (the all-null
spelling). Each case: nanolance writes the table; pylance and nanolance read it back equal, whole, by row range and by
take. Where pylance 12 can write the shape, pylance's own file reads back equal in nanolance too.

The layout is pylance's for a small column: the same Constant kind, layers, and raw Flat(16) definition buffer, with the
same buffer sizes (checked on 6-row tables, against pylance's file). Where the levels are all equal pylance stores them as
RLE, and nanolance writes the same raw levels; the page is otherwise the same. A large column differs in two more choices
that decode the same rows: nanolance cuts its null-typed pages at 1024 rows, where pylance writes one page, and pylance
picks Bitpacked or RLE for the levels by size. Shapes pylance refuses to write (a struct row that is null over a null
child) are checked by the round trip alone.
"""

from __future__ import annotations

import re

import nanolance
import pyarrow as pa
import pytest
from tests.support import require_pylance
from tests.test_dict_64bit import _layout

S_AB = pa.struct([("a", pa.null()), ("b", pa.int64())])
S_XN = pa.struct([("a", pa.struct([("x", pa.null())]))])
LS_NULL_UTF8 = pa.list_(pa.struct([("a", pa.null()), ("b", pa.utf8())]))


def _rep(pattern, n):
    """`n` rows cycling through `pattern`: null rows, empty lists, lists of nulls, all in one column."""
    return [pattern[i % len(pattern)] for i in range(n)]


def _cases(n):
    return {
        "list_null": pa.array(
            _rep([[], [None], None, [None, None]], n), pa.list_(pa.null())
        ),
        "large_list_null": pa.array(
            _rep([[], [None], None, [None, None, None]], n), pa.large_list(pa.null())
        ),
        "list_null_empty_lists": pa.array(_rep([[], []], n), pa.list_(pa.null())),
        "list_list_null": pa.array(
            _rep([[[None], []], None, [], [[], [None, None]]], n),
            pa.list_(pa.list_(pa.null())),
        ),
        "struct_null_valid_rows": pa.array(
            _rep([{"a": None, "b": 1}, {"a": None, "b": None}], n), S_AB
        ),
        "struct_null_rows": pa.array(
            _rep([{"a": None, "b": 1}, None, {"a": None, "b": None}], n), S_AB
        ),
        "struct_struct_null": pa.array(
            _rep([{"a": {"x": None}}, {"a": {"x": None}}], n), S_XN
        ),
        "struct_struct_null_rows": pa.array(
            _rep([{"a": {"x": None}}, None, {"a": None}], n), S_XN
        ),
        "list_struct_null_valid_items": pa.array(
            _rep(
                [
                    [{"a": None, "b": "x"}],
                    [],
                    [{"a": None, "b": None}, {"a": None, "b": "y"}],
                ],
                n,
            ),
            LS_NULL_UTF8,
        ),
        "list_struct_null_items": pa.array(
            _rep(
                [[{"a": None, "b": "x"}], None, [], [None, {"a": None, "b": None}]], n
            ),
            LS_NULL_UTF8,
        ),
        "struct_list_null": pa.array(
            _rep([{"l": []}, None, {"l": [None, None]}, {"l": None}, {"l": [None]}], n),
            pa.struct([("l", pa.list_(pa.null()))]),
        ),
        "map_null": pa.array(
            _rep([[("k", None)], None, []], n), pa.map_(pa.utf8(), pa.null())
        ),
        "all_null_rows": pa.array([None] * n, pa.list_(pa.null())),
    }


# 2000 rows: more than one page's worth of 1024, so the column spans pages.
CASES = _cases(2000)
# 6 rows: small enough that pylance's page and level encodings are the raw ones this writer uses.
SMALL_CASES = _cases(6)

# The shapes pylance 12 writes (at format 2.2): their null-typed pages are compared with pylance's own layout.
PYLANCE_WRITES = {
    "list_null",
    "list_null_empty_lists",
    "large_list_null",
    "list_list_null",
    "struct_null_valid_rows",
    "struct_struct_null",
    "list_struct_null_valid_items",
    "struct_list_null",
    "map_null",
    "all_null_rows",
}

# Of those, the shapes whose levels are all equal, which pylance stores as RLE rather than raw.
RUN_LEVELS = {
    "struct_null_valid_rows",
    "struct_struct_null",
    "all_null_rows",
    "list_null_empty_lists",
}


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _table(cases, name):
    return pa.table(
        {"c": cases[name], "id": pa.array(range(len(cases[name])), pa.int64())}
    )


def _check_read(path, expected):
    """pylance and nanolance read `path` back equal to `expected`: whole, by row range, by take and by projection."""
    import lance

    ds = lance.dataset(path)
    n = expected.num_rows
    assert ds.to_table().equals(expected)
    assert pa.table(nanolance.read_table(path)).equals(expected)

    rows = [n - 1, 0, n // 2]
    assert pa.table(nanolance.take(path, rows)).equals(expected.take(rows))
    assert ds.take(rows).equals(expected.take(rows))

    for offset, length in [
        (0, min(5, n)),
        (n // 3, min(700, n - n // 3)),
        (max(0, n - 4), 4),
    ]:
        ranged = expected.slice(offset, length)
        assert pa.table(
            nanolance.read_table(path, offset=offset, length=length)
        ).equals(ranged)
        assert ds.to_table(offset=offset, limit=length).equals(ranged)

    assert pa.table(nanolance.read_table(path, columns=["c"])).equals(
        expected.select(["c"])
    )


def _null_layout(path):
    """nlance-pagelayout's lines for the null-typed pages only (its leaf is named with the type `(null)`)."""
    return [line for line in _layout(path) if "(null)" in line]


@pytest.mark.parametrize("name", sorted(CASES))
def test_nanolance_writes_null_children_round_trip(lance_mod, tmp_path, name):
    table = _table(CASES, name)
    path = str(tmp_path / f"{name}.lance")
    nanolance.write_table(table, path)
    _check_read(path, table)


@pytest.mark.parametrize("name", sorted(PYLANCE_WRITES - RUN_LEVELS))
def test_null_children_layout_matches_pylance(lance_mod, tmp_path, name):
    table = _table(SMALL_CASES, name)
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nanolance.write_table(table, str(ours))
    lance_mod.write_dataset(table, str(theirs), data_storage_version="2.2")

    assert _null_layout(ours), "the shape has no null-typed page to compare"
    assert _null_layout(ours) == _null_layout(theirs)


@pytest.mark.parametrize("name", sorted(RUN_LEVELS))
def test_null_children_constant_kind_matches_pylance(lance_mod, tmp_path, name):
    """Levels that are all equal: pylance stores their definition buffer as RLE, nanolance as raw Flat(16). The page is
    the same Constant with the same layers either way; only the definition buffer's encoding is named differently."""
    table = _table(SMALL_CASES, name)
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nanolance.write_table(table, str(ours))
    lance_mod.write_dataset(table, str(theirs), data_storage_version="2.2")

    def kind(lines):
        return [
            re.sub(
                r"def=(Rle\{[^}]*\}|Flat\(16\)|Bitpacked\(16,Flat\(\d+\)\))",
                "def=*",
                line,
            )
            for line in lines
        ]

    assert _null_layout(ours), "the shape has no null-typed page to compare"
    assert kind(_null_layout(ours)) == kind(_null_layout(theirs))


@pytest.mark.parametrize("name", sorted(PYLANCE_WRITES))
def test_pylance_written_null_children_read_by_nanolance(lance_mod, tmp_path, name):
    table = _table(CASES, name)
    theirs = tmp_path / "theirs.lance"
    lance_mod.write_dataset(table, str(theirs), data_storage_version="2.2")
    _check_read(str(theirs), table)
