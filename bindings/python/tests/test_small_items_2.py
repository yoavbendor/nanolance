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


# ---------------------------------------------------------------------------------------------
# Phase B: plans and stats (items 4, 6, 7; items 3 and 5 are not in this file yet)

import re  # noqa: E402

import nanolance.lance as nano  # noqa: E402


def test_b4_analyze_filtered_scan_reports_rows_read(tmp_path):
    # The line pylance's test_analyze_filtered_scan searches for, on nanolance's own plan text.
    ds = nano.write_dataset(pa.Table.from_pydict({"a": range(100), "b": range(100)}), str(tmp_path / "ds"))
    plan = ds.scanner(columns=[], filter="a < 50", with_row_id=True).analyze_plan()
    assert re.search(r"^\s*LanceRead:.*output_rows=50.*rows_scanned=100.*$", plan, re.MULTILINE), plan


def test_b4_analyze_plan_with_index_does_not_claim_rows_scanned(tmp_path):
    ds = nano.write_dataset(pa.Table.from_pydict({"a": range(100)}), str(tmp_path / "ds"))
    ds.create_scalar_index("a", "BTREE")
    plan = ds.scanner(columns=[], filter="a < 50", with_row_id=True).analyze_plan()
    assert "ScalarIndexQuery" in plan
    assert "output_rows=50" in plan
    assert "rows_scanned" not in plan


@pytest.mark.parametrize(
    ("late_materialization", "is_late"),
    [(None, False), (False, False), (True, True), (["values"], True), (["filter"], False)],
    ids=["default", "all_early", "all_late", "late_column", "early_column"],
)
def test_b6_late_materialization_plan_lines_and_results(tmp_path, late_materialization, is_late):
    lance = require_pylance()
    table = pa.table({"filter": range(2000), "values": range(2000)})
    ours = nano.write_dataset(table, str(tmp_path / "ds"), data_storage_version="stable")
    theirs = lance.write_dataset(table, str(tmp_path / "theirs"), data_storage_version="stable")
    # Both engines: the same late read (one `source=stream`) for the same parameter.
    for ds in (ours, theirs):
        plan = ds.scanner(filter="filter % 2 == 0", late_materialization=late_materialization).explain_plan(True)
        assert plan.count("source=stream") == (1 if is_late else 0), plan
    expected = theirs.to_table(filter="filter % 2 == 0")
    assert ours.to_table(filter="filter % 2 == 0", late_materialization=late_materialization) == expected


def _late_by_default(lance, path, column_type, values):
    """Whether a filtered scan reads `values` late under the default (late_materialization=None)."""
    table = pa.table({"filter": range(len(values)), "values": pa.array(values, column_type)})
    ds = lance.write_dataset(table, str(path), data_storage_version="stable", max_rows_per_file=10000)
    plan = ds.scanner(filter="filter % 2 == 0", late_materialization=None).explain_plan(True)
    return "source=stream" in plan


@pytest.mark.parametrize(
    ("column_type", "values"),
    [
        (pa.int64(), [1, 2, 3, 4]),
        (pa.int8(), [1, 2, 3, 4]),
        (pa.float16(), [1, 2, 3, 4]),
        (pa.date32(), [1, 2, 3, 4]),
        (pa.timestamp("us"), [1, 2, 3, 4]),
        (pa.duration("ms"), [1, 2, 3, 4]),
        (pa.binary(4), [b"abcd"] * 4),
        (pa.binary(16), [b"abcdefghijklmnop"] * 4),
        (pa.decimal128(10, 2), [1, 2, 3, 4]),
        (pa.bool_(), [True, False, True, False]),
        (pa.string(), ["a", "b", "c", "d"]),
        (pa.binary(), [b"a", b"b", b"c", b"d"]),
        (pa.list_(pa.int64()), [[1], [2], [3], [4]]),
        (pa.list_(pa.string()), [["a"], ["b"], ["c"], ["d"]]),
        (pa.struct([("x", pa.int64())]), [{"x": 1}] * 4),
        (pa.struct([("x", pa.string())]), [{"x": "a"}] * 4),
        (pa.list_(pa.int64(), 2), [[1, 2]] * 4),
        (pa.list_(pa.float32(), 2), [[1.0, 2.0]] * 4),
    ],
    ids=lambda v: str(v) if isinstance(v, pa.DataType) else None,
)
def test_b6_default_late_materialization_by_type_equals_pylance(tmp_path, column_type, values):
    lance = require_pylance()
    table = pa.table({"filter": range(len(values)), "values": pa.array(values, column_type)})
    ours = nano.write_dataset(table, str(tmp_path / "ds"), data_storage_version="stable", max_rows_per_file=10000)
    theirs = lance.write_dataset(table, str(tmp_path / "theirs"), data_storage_version="stable",
                                 max_rows_per_file=10000)
    late = "source=stream" in ours.scanner(filter="filter % 2 == 0", late_materialization=None).explain_plan(True)
    assert late == _late_by_default(lance, tmp_path / "pylance_only", column_type, values)
    assert ours.to_table(filter="filter % 2 == 0") == theirs.to_table(filter="filter % 2 == 0")


def test_b6_late_materialization_rejects_bad_values(tmp_path):
    ds = nano.write_dataset(pa.table({"a": range(10)}), str(tmp_path / "ds"))
    with pytest.raises(ValueError, match="late_materialization must be a bool or a list of strings"):
        ds.scanner(late_materialization=123)


def _metric(analysis, name):
    return int(re.search(rf"\b{name}=(\d+)", analysis).group(1))


def test_b7_merge_insert_plan_and_dry_run_counts(tmp_path):
    lance = require_pylance()
    data = pa.table({"id": [1, 2, 3, 4], "value": ["a", "b", "c", "d"]})
    ours = nano.write_dataset(data, str(tmp_path / "ours"))
    theirs = lance.write_dataset(data, str(tmp_path / "theirs"))
    source = pa.table({"id": [1, 5], "value": ["updated_a", "e"]})

    def builder(ds):
        return ds.merge_insert("id").when_matched_update_all().when_not_matched_insert_all()

    plan = builder(ours).explain_plan()
    assert plan.startswith("MergeInsert: on=[id], when_matched=UpdateAll, when_not_matched=InsertAll")
    assert "when_not_matched_by_source=Keep" in plan
    assert "MergeInsert" in builder(theirs).explain_plan()
    assert "write_mode=auto" in builder(ours).explain_plan(verbose=True)

    # analyze_plan writes the merge's files but commits nothing: the version is unchanged and the
    # written files are on disk, unreferenced (as Lance leaves them).
    data_dir = tmp_path / "ours" / "data"
    before = set(data_dir.iterdir())
    version = ours.version
    analysis = builder(ours).analyze_plan(source)
    written = set(data_dir.iterdir()) - before
    assert ours.version == version and lance.dataset(str(tmp_path / "ours")).version == version
    assert ours.count_rows() == 4
    assert len(written) == 1 and sum(p.stat().st_size for p in written) == _metric(analysis, "bytes_written")
    assert "nothing written" not in analysis

    # The counts equal pylance's for the same merge.
    theirs_analysis = builder(theirs).analyze_plan(source)
    for name in ("num_inserted_rows", "num_updated_rows", "num_deleted_rows", "num_files_written"):
        assert _metric(analysis, name) == _metric(theirs_analysis, name), name


def test_b7_merge_insert_delete_counts_equal_pylance(tmp_path):
    lance = require_pylance()
    data = pa.table({"id": [1, 2, 3, 4], "value": ["a", "b", "c", "d"]})
    ours = nano.write_dataset(data, str(tmp_path / "ours"))
    theirs = lance.write_dataset(data, str(tmp_path / "theirs"))
    keys = pa.table({"id": [1, 2]})
    ours_analysis = ours.merge_insert("id").when_matched_delete().analyze_plan(keys)
    theirs_analysis = theirs.merge_insert("id").when_matched_delete().analyze_plan(keys)
    assert _metric(ours_analysis, "num_deleted_rows") == _metric(theirs_analysis, "num_deleted_rows") == 2
    assert _metric(ours_analysis, "num_files_written") == _metric(theirs_analysis, "num_files_written") == 0
    assert ours.count_rows() == 4 and ours.version == 1
