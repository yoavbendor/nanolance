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


def _fsl_table(rows: int, n: int, shape: str, seed: int = 1) -> pa.Table:
    """fixed_size_list<float32, n> with null ELEMENTS (one in five) under a list (3 items a row) or a struct."""
    rng = np.random.default_rng(seed)
    total = rows * 3 if shape == "list" else rows
    values = pa.array(rng.standard_normal(total * n).astype(np.float32), mask=rng.random(total * n) < 0.2)
    fsl = pa.FixedSizeListArray.from_arrays(values, n)
    if shape == "list":
        return pa.table({"l": pa.ListArray.from_arrays(pa.array(np.arange(0, total + 1, 3, dtype=np.int32)), fsl)})
    return pa.table({"s": pa.StructArray.from_arrays([fsl, pa.array(np.arange(rows))], names=["f", "i"])})


FSL_CASES = [("list", 3_000), ("struct", 3_000), ("list", 300_000), ("struct", 300_000)]


@pytest.mark.parametrize("shape,rows", FSL_CASES, ids=[f"{s}-{r}" for s, r in FSL_CASES])
def test_fsl_element_nulls_match_pylance(tmp_path, shape, rows):
    """fixed_size_list<float32, 2> with null elements under a list or struct: pylance's layout (FixedSizeList with
    validity, two buffers a chunk), read back by both, take included."""
    lance = require_pylance()
    table = _fsl_table(rows, 2, shape)
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs), data_storage_version="2.2")

    expected = table.to_pylist()
    assert nl_dataset(str(ours)).to_table().to_pylist() == expected
    assert lance.dataset(str(ours)).to_table().to_pylist() == expected
    assert _layout(ours) == _layout(theirs)

    wanted = sorted(np.random.default_rng(4).choice(rows, size=min(200, rows), replace=False).tolist())
    assert lance.dataset(str(ours)).take(wanted).to_pylist() == [expected[i] for i in wanted]


@pytest.mark.parametrize("shape", ["list", "struct"])
def test_fsl_element_nulls_append_to_pylance_dataset(tmp_path, shape):
    lance = require_pylance()
    first = _fsl_table(3_000, 2, shape, seed=1)
    second = _fsl_table(1_000, 2, shape, seed=2)
    path = str(tmp_path / "pylance_first.lance")
    lance.write_dataset(first, path, data_storage_version="2.2")
    nl_write_dataset(second, path, mode="append")
    expected = first.to_pylist() + second.to_pylist()
    assert lance.dataset(path).to_table().to_pylist() == expected
    assert nl_dataset(path).to_table().to_pylist() == expected


@pytest.mark.parametrize("nulls", [True, False])
@pytest.mark.parametrize("shape", ["list", "struct"])
def test_fsl_of_256_bytes_or_more_is_nested_full_zip(tmp_path, shape, nulls):
    """Rows of 256 bytes or more under a list or struct are FullZip in pylance (Lance's is_narrow rule), with or
    without element nulls: a control word per level, a slot per item, a repetition index under a list. The layout
    text equals pylance's and both read it back, take included."""
    lance = require_pylance()
    table = _fsl_table(2_000, 256, shape)
    if not nulls:
        table = pa.Table.from_pylist([_fill_nulls(r) for r in table.to_pylist()], schema=table.schema)
    ours, theirs = str(tmp_path / "ours.lance"), str(tmp_path / "theirs.lance")
    nl_write_dataset(table, ours)
    lance.write_dataset(table, theirs)
    layout = _layout(ours)
    assert any("FullZip" in line for line in layout), layout
    assert layout == _layout(theirs)
    assert nl_dataset(ours).to_table() == table
    assert lance.dataset(ours).to_table() == table
    picks = [0, 1, 7, 1_000, 1_999]
    assert lance.dataset(ours).take(picks) == table.take(picks)
    assert nl_dataset(ours).take(picks) == table.take(picks)


def _fill_nulls(value):
    """`value` with every null element of a fixed_size_list replaced by 0.0 (rows and lists stay as they are)."""
    if isinstance(value, dict):
        return {k: _fill_nulls(v) for k, v in value.items()}
    if isinstance(value, list):
        return [0.0 if v is None and (not value or all(not isinstance(x, (list, dict)) for x in value)) else
                _fill_nulls(v) for v in value]
    return value


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
