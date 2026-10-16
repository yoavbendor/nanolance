"""Small items bundle 2, phase A: Bitmap construction checks and decimal / duration scalar indexes.

Each section compares nanolance with pylance 12.0.0 directly, the way the other engine tests do.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

from nanolance.lance.bitmap import Bitmap as NanoBitmap
from tests.support import require_pylance

# ---------------------------------------------------------------------------------------------
# 1. lance.bitmap.Bitmap construction checks


def _error(make):
    try:
        make()
    except Exception as exc:  # noqa: BLE001 -- the type and text are what is compared
        return type(exc), str(exc)
    return None


@pytest.mark.parametrize(
    "values",
    [[-1], [2**32], [-7], [1.5], [1.0], ["x"], [None], [1, "a"]],
    ids=["negative", "too_large", "negative_seven", "float", "float_whole", "str", "none", "mixed"],
)
def test_bitmap_constructor_errors_equal_pylance(values):
    lance = require_pylance()
    assert _error(lambda: NanoBitmap(values)) == _error(lambda: lance.bitmap.Bitmap(values))


@pytest.mark.parametrize(
    "make",
    [
        lambda b: b.update([-1]),
        lambda b: b.update([1.5]),
        lambda b: b.union([2**32]),
        lambda b: b.intersection([-1]),
        lambda b: b.difference([-1]),
        lambda b: b.issubset([-1]),
    ],
    ids=["update_neg", "update_float", "union_big", "intersection_neg", "difference_neg", "issubset_neg"],
)
def test_bitmap_bulk_operations_errors_equal_pylance(make):
    lance = require_pylance()
    assert _error(lambda: make(NanoBitmap([1]))) == _error(lambda: make(lance.bitmap.Bitmap([1])))


@pytest.mark.parametrize(
    "op",
    [lambda b: b.add(-1), lambda b: b.add(2**32), lambda b: b.discard(-1), lambda b: b.remove(-1),
     lambda b: b.add(1.5)],
    ids=["add_neg", "add_big", "discard_neg", "remove_neg", "add_float"],
)
def test_bitmap_single_value_errors_equal_pylance(op):
    lance = require_pylance()
    assert _error(lambda: op(NanoBitmap([1]))) == _error(lambda: op(lance.bitmap.Bitmap([1])))


def test_bitmap_accepts_the_u32_bounds_and_numpy_ints():
    import numpy as np

    lance = require_pylance()
    for make in (NanoBitmap, lance.bitmap.Bitmap):
        assert list(make([0, 2**32 - 1, np.int64(7), np.uint32(9), True])) == [0, 1, 7, 9, 2**32 - 1]


@pytest.mark.parametrize(
    ("array", "expected"),
    [
        (pa.array([1, 2, None], pa.int32()), "containing nulls"),
        (pa.array([1.0, 2.0], pa.float64()), "integer pyarrow array, got Float64"),
        (pa.array(["a"]), "integer pyarrow array, got Utf8"),
        (pa.array([-1], pa.int32()), "unsigned 32-bit integer: Cast error: Can't cast value -1 "),
        (pa.array([2**33], pa.int64()), "value 8589934592 "),
        (pa.array([2**64 - 1], pa.uint64()), "value 18446744073709551615 "),
        (pa.chunked_array([pa.array([1, None])]), "containing nulls"),
    ],
    ids=["nulls", "float64", "utf8", "int32_neg", "int64_big", "uint64_big", "chunked_nulls"],
)
def test_bitmap_from_arrow_errors_equal_pylance(array, expected):
    lance = require_pylance()
    nano, theirs = _error(lambda: NanoBitmap(array)), _error(lambda: lance.bitmap.Bitmap(array))
    assert nano == theirs and nano is not None and nano[0] is ValueError
    assert expected in theirs[1]


def test_bitmap_from_arrow_values_equal_pylance():
    lance = require_pylance()
    for array in (pa.array([], pa.int32()), pa.array([1, 2], pa.int8()), pa.array([2**32 - 1], pa.uint32()),
                  pa.chunked_array([pa.array([3]), pa.array([4, 5])])):
        assert list(NanoBitmap(array)) == list(lance.bitmap.Bitmap(array))


# ---------------------------------------------------------------------------------------------
# 2. decimal128 / duration scalar indexes: the index files equal pylance's. The filters over these
# columns (arrow_cast to Decimal / Duration) are not in this section yet.

INDEX_KINDS = ["BTREE", "BITMAP", "ZONEMAP"]


def _typed_table(kind):
    from decimal import Decimal

    if kind == "decimal128":
        values = [Decimal("1.00"), Decimal("2.50"), Decimal("-3.25"), None, Decimal("2.50")]
        return pa.table({"value": pa.array(values, pa.decimal128(10, 2))})
    return pa.table({"value": pa.array([1, 2, 3, None, 2], pa.duration("ms"))})


def _index_pages(lance, path, name):
    """The index's files as {file name: table}, read with pylance's file reader."""
    import os

    from lance.file import LanceFileReader

    (index,) = [i for i in lance.dataset(path).list_indices() if i["name"] == name]
    base = f"{path}/_indices/{index['uuid']}"
    return {f: LanceFileReader(f"{base}/{f}").read_all().to_table()
            for f in sorted(os.listdir(base)) if f.endswith(".lance")}


@pytest.mark.parametrize("index_type", INDEX_KINDS)
@pytest.mark.parametrize("kind", ["decimal128", "duration"])
def test_decimal_and_duration_index_files_equal_pylance(tmp_path, index_type, kind):
    lance = require_pylance()
    table = _typed_table(kind)
    name = f"{index_type.lower()}_{kind}"
    for who in ("nanolance", "pylance"):
        path = str(tmp_path / f"{who}.lance")
        lance.write_dataset(table, path)
        if who == "nanolance":
            from nanolance.lance import dataset as nano_dataset

            nano_dataset(path).create_scalar_index("value", index_type, name=name)
        else:
            lance.dataset(path).create_scalar_index("value", index_type, name=name)
    ours = _index_pages(lance, str(tmp_path / "nanolance.lance"), name)
    theirs = _index_pages(lance, str(tmp_path / "pylance.lance"), name)
    assert ours.keys() == theirs.keys()
    for file in ours:
        assert ours[file].equals(theirs[file]), file
