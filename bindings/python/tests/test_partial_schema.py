"""Writes with part of the schema, against pylance.

An append of some of the dataset's columns writes files that hold those columns alone, as Lance
does, and the others read as null -- a struct or list column as a null value, not a value of nulls.
A merge insert whose source lacks columns keeps the matched rows' values there and gives inserted
rows nulls. A merge insert without ``on`` uses the schema's unenforced primary key. Each case runs
on nanolance and on pylance and the tables must be equal, whichever library reads them.
"""

from __future__ import annotations

import shutil

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

SCHEMA = pa.schema([
    pa.field("a", pa.int64(), nullable=False),
    pa.field("b", pa.string()),
    pa.field("s", pa.struct([("x", pa.int64()), ("y", pa.string())])),
    pa.field("l", pa.list_(pa.int32())),
    pa.field("ls", pa.list_(pa.struct([("q", pa.string())]))),
    pa.field("ss", pa.struct([("i", pa.struct([("z", pa.float32())]))])),
    pa.field("f", pa.list_(pa.float32(), 3)),
])


def _base(n=10):
    return pa.table({
        "a": list(range(n)),
        "b": [str(i) for i in range(n)],
        "s": [{"x": i, "y": "v"} if i % 3 else None for i in range(n)],
        "l": [[i] * (i % 4) if i % 5 else None for i in range(n)],
        "ls": [[{"q": str(i)}] for i in range(n)],
        "ss": [{"i": {"z": float(i)}} for i in range(n)],
        "f": [[1.0, 2.0, float(i)] for i in range(n)],
    }, schema=SCHEMA)


def _both(tmp_path, act):
    """`act(lib, path)` on a nanolance and a pylance dataset; both read back by both libraries."""
    lance = require_pylance()
    paths = []
    for lib, name in ((nl, "n"), (lance, "p")):
        path = str(tmp_path / f"{name}.lance")
        shutil.rmtree(path, ignore_errors=True)
        lib.write_dataset(_base(), path, max_rows_per_file=4)
        act(lib, path)
        paths.append(path)
    tables = [lib.dataset(p).to_table().sort_by("a") for p in paths for lib in (nl, lance)]
    for t in tables[1:]:
        assert t.equals(tables[0])
    return tables[0]


def test_append_part_of_the_schema(tmp_path):
    def act(lib, path):
        lib.dataset(path).insert(pa.table({"a": [100, 101]}))
        lib.write_dataset(pa.table({"l": pa.array([[7]], pa.list_(pa.int32())), "a": [102]}), path, mode="append")

    table = _both(tmp_path, act)
    rows = {r["a"]: r for r in table.to_pylist()}
    assert rows[100]["s"] is None and rows[100]["ss"] is None and rows[100]["l"] is None
    assert rows[102]["l"] == [7] and rows[102]["b"] is None


def test_append_files_hold_only_the_given_columns(tmp_path):
    lance = require_pylance()
    path = str(tmp_path / "n.lance")
    nl.write_dataset(_base(), path)
    nl.dataset(path).insert(pa.table({"a": [100], "b": ["x"]}))
    files = lance.dataset(path).get_fragments()[-1].metadata.files
    assert [f.fields for f in files] == [[0, 1]]


def test_take_and_filter_over_missing_columns(tmp_path):
    lance = require_pylance()
    path = str(tmp_path / "n.lance")
    nl.write_dataset(_base(), path)
    nl.dataset(path).insert(pa.table({"a": [100, 101]}))
    ours, theirs = nl.dataset(path), lance.dataset(path)
    assert ours.take([11, 3, 10]).equals(theirs.take([11, 3, 10]))
    assert ours.to_table(filter="a > 5").equals(theirs.to_table(filter="a > 5"))


@pytest.mark.parametrize("given,missing", [
    (pa.table({"b": ["x"]}), "a"),
    (pa.table({"a": [1], "zz": [1]}), None),
])
def test_append_refusals_as_lance(tmp_path, given, missing):
    lance = require_pylance()
    messages = []
    for lib, name in ((nl, "n"), (lance, "p")):
        path = str(tmp_path / f"{name}.lance")
        lib.write_dataset(_base(), path)
        with pytest.raises(OSError) as info:
            lib.write_dataset(given, path, mode="append")
        messages.append(str(info.value).split(", location")[0])
    assert messages[0] == messages[1]


@pytest.mark.parametrize("on,source", [
    ("a", pa.table({"a": [3, 4, 42], "b": ["u", "v", None]})),
    ("a", pa.table({"l": pa.array([[9], None], pa.list_(pa.int32())), "a": [1, 7]})),
    (["a", "b"], pa.table({"a": [3, 5], "b": ["3", "0"],
                           "f": pa.array([[0.0, 0.0, 0.0]] * 2, pa.list_(pa.float32(), 3))})),
])
def test_merge_insert_part_of_the_schema(tmp_path, on, source):
    stats = []

    def act(lib, path):
        ds = lib.dataset(path)
        ds.delete("a = 8")
        stats.append(dict(ds.merge_insert(on).when_matched_update_all().when_not_matched_insert_all()
                          .execute(source)))

    _both(tmp_path, act)
    assert stats[0] == stats[1]


def test_merge_insert_when_matched_delete_and_fail(tmp_path):
    stats = []

    def act(lib, path):
        ds = lib.dataset(path)
        stats.append(dict(ds.merge_insert("a").when_matched_delete().execute(pa.table({"a": [1, 2, 99]}))))
        with pytest.raises(OSError):
            ds.merge_insert("a").when_matched_fail().when_not_matched_insert_all().execute(_base(1))
        stats.append(dict(ds.merge_insert("a").when_matched_fail().when_not_matched_insert_all()
                          .execute(pa.table({"a": [50]}))))

    _both(tmp_path, act)
    assert stats[:2] == stats[2:]


def test_merge_insert_by_primary_key(tmp_path):
    lance = require_pylance()
    schema = pa.schema([
        pa.field("id", pa.int32(), nullable=False, metadata={b"lance-schema:unenforced-primary-key": b"true"}),
        pa.field("value", pa.int32()),
    ])
    for lib, name in ((nl, "n"), (lance, "p")):
        path = str(tmp_path / f"{name}.lance")
        ds = lib.write_dataset(pa.table({"id": [1, 2, 3], "value": [10, 20, 30]}, schema=schema), path)
        stats = ds.merge_insert().when_matched_update_all().when_not_matched_insert_all().execute(
            pa.table({"id": [2, 3, 4], "value": [200, 300, 400]}, schema=schema))
        assert (stats["num_inserted_rows"], stats["num_updated_rows"]) == (1, 2)
        assert lib.dataset(path).to_table().sort_by("id").column("value").to_pylist() == [10, 200, 300, 400]
        plain = lib.write_dataset(pa.table({"id": [1]}), str(tmp_path / f"{name}2.lance"))
        with pytest.raises(ValueError, match="join keys"):
            plain.merge_insert()


def test_merge_insert_that_changes_nothing_is_refused(tmp_path):
    path = str(tmp_path / "n.lance")
    ds = nl.write_dataset(_base(), path)
    with pytest.raises(ValueError, match="not configured to change the data"):
        ds.merge_insert("a").execute(_base(2))
