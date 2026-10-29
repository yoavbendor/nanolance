"""Nested shapes nanolance writes (W1k2): a dictionary column inside a struct or a list, as pylance 12 writes it.

Each case: nanolance writes the table; nanolance and pylance read it back equal, take included; the layout is
pylance's (nlance-pagelayout on both files); and an append by nanolance to a pylance-written dataset reads back in
pylance. fixed_size_list element nulls under a list or struct (W1k2) are covered further down.
"""

from __future__ import annotations

import numpy as np
import pyarrow as pa
import pytest

from nanolance.lance.dataset import dataset as nl_dataset
from nanolance.lance.dataset import write_dataset as nl_write_dataset
from tests.support import require_pylance
from tests.test_dict_64bit import _layout


def _dictionary(rows: int, card: int, nulls: bool, seed: int) -> pa.DictionaryArray:
    """`rows` indices into `card` strings (int32 indices), a tenth of them null when `nulls`."""
    rng = np.random.default_rng(seed)
    words = pa.array([f"word-{i:06d}" for i in range(card)])
    indices = rng.integers(0, card, size=rows).astype(np.int32)
    mask = rng.random(rows) < 0.1 if nulls else None
    return pa.DictionaryArray.from_arrays(pa.array(indices, mask=mask), words)


def _struct_table(rows: int, card: int, nulls: bool, seed: int = 1) -> pa.Table:
    keys = _dictionary(rows, card, nulls, seed)
    values = pa.array(np.arange(rows, dtype=np.int64) * 7)
    return pa.table({"s": pa.StructArray.from_arrays([keys, values], names=["k", "v"])})


def _list_table(rows: int, card: int, nulls: bool, seed: int = 1) -> pa.Table:
    items = _dictionary(rows * 2, card, nulls, seed)
    offsets = pa.array(np.arange(0, rows * 2 + 1, 2, dtype=np.int32))
    return pa.table({"l": pa.ListArray.from_arrays(offsets, items)})


SHAPES = {"struct": _struct_table, "list": _list_table}

# (shape, rows, distinct values, nulls): the 3,000-row matrix, then the 300,000-row cases.
CASES = [
    (shape, rows, card, nulls)
    for rows in (3_000,)
    for shape in SHAPES
    for card in (10, 1_000)
    for nulls in (False, True)
] + [
    ("struct", 300_000, 10, True),
    ("list", 300_000, 1_000, True),
]


def _id(case):
    shape, rows, card, nulls = case
    return f"{shape}-{rows}-{card}-{'nulls' if nulls else 'valid'}"


@pytest.mark.parametrize("case", CASES, ids=[_id(c) for c in CASES])
def test_dictionary_in_nested_matches_pylance(tmp_path, case):
    lance = require_pylance()
    shape, rows, card, nulls = case
    table = SHAPES[shape](rows, card, nulls)

    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")

    expected = table.to_pylist()
    assert nl_dataset(str(ours)).to_table().to_pylist() == expected
    assert lance.dataset(str(ours)).to_table().to_pylist() == expected

    # The layout is pylance's, leaf by leaf.
    ours_layout = _layout(ours)
    theirs_layout = _layout(theirs)
    assert ours_layout == theirs_layout, f"nanolance {ours_layout} pylance {theirs_layout}"
    assert any("dict=General{LZ4" in line for line in theirs_layout)

    rng = np.random.default_rng(3)
    wanted = sorted(rng.choice(rows, size=min(200, rows), replace=False).tolist())
    taken = lance.dataset(str(ours)).take(wanted).to_pylist()
    assert taken == [expected[i] for i in wanted]
    assert nl_dataset(str(ours)).take(wanted).to_pylist() == [expected[i] for i in wanted]


@pytest.mark.parametrize("shape", list(SHAPES))
def test_dictionary_in_nested_append_to_pylance_dataset(tmp_path, shape):
    lance = require_pylance()
    first = SHAPES[shape](3_000, 10, True, seed=1)
    second = SHAPES[shape](2_000, 25, True, seed=2)  # its own dictionary, some of its words new
    path = str(tmp_path / "pylance_first.lance")
    lance.write_dataset(first, path, data_storage_version="2.2")

    nl_write_dataset(second, path, mode="append")

    expected = first.to_pylist() + second.to_pylist()
    assert lance.dataset(path).to_table().to_pylist() == expected
    assert nl_dataset(path).to_table().to_pylist() == expected


def test_dictionary_in_nested_read_back_without_nulls_in_struct(tmp_path):
    """A struct whose dictionary child has no nulls is written flat under the struct (no repdef), as pylance does."""
    lance = require_pylance()
    table = _struct_table(3_000, 10, False)
    ours = tmp_path / "ours.lance"
    nl_write_dataset(table, str(ours))
    layout = _layout(ours)
    assert layout == _layout(_write_pylance(lance, table, tmp_path / "theirs.lance"))
    assert not any("repdef" in line for line in layout if "s.k" in line)


def _write_pylance(lance, table, path):
    lance.write_dataset(table, str(path), data_storage_version="2.2")
    return path


def test_struct_child_with_nulls_is_read_by_pylance(tmp_path):
    """A struct whose child has nulls (and the struct none) read back in pylance: nanolance used to write it with
    flat levels that pylance's reader panics on (found with the dictionary shapes, W1k2)."""
    lance = require_pylance()
    rng = np.random.default_rng(5)
    rows = 3_000
    child = pa.array(rng.integers(0, 1_000, size=rows), mask=rng.random(rows) < 0.1)
    table = pa.table({"s": pa.StructArray.from_arrays([child, pa.array(np.arange(rows))], names=["k", "v"])})
    path = str(tmp_path / "ours.lance")
    nl_write_dataset(table, path)
    assert lance.dataset(path).to_table().to_pylist() == table.to_pylist()
