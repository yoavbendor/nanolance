"""Small dataset APIs against pylance: stats, lance_schema, update_field_metadata, transactions,
merge, validate.

Each test does the same thing with both libraries (or reads one library's dataset with both) and
compares what they report.
"""

from __future__ import annotations

import dataclasses
import pickle

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


def _table(n=2000, seed=0):
    rng = np.random.default_rng(seed)
    return pa.table({
        "id": range(n),
        "c": [i % 5 for i in range(n)],
        "f": rng.random(n),
        "s": [f"hello w{i % 7}" for i in range(n)],
        "l": [[f"a{i % 3}"] for i in range(n)],
        "v": pa.FixedSizeListArray.from_arrays(pa.array(rng.random(n * 8).astype("float32")), 8),
    })


def _indexed(lib, path):
    t = _table()
    ds = lib.write_dataset(t, path, max_rows_per_file=500)
    ds.create_scalar_index("id", "BTREE")
    ds.create_scalar_index("f", "BTREE")
    ds.create_scalar_index("c", "BITMAP")
    ds.create_scalar_index("s", "INVERTED")
    ds.create_scalar_index("l", "LABEL_LIST")
    lib.dataset(path).create_index("v", "IVF_PQ", num_partitions=2, num_sub_vectors=2)
    lib.write_dataset(t.slice(0, 10), path, mode="append")
    lib.dataset(path).delete("id < 3")
    return lib.dataset(path)


INDICES = ["id_idx", "f_idx", "c_idx", "s_idx", "l_idx", "v_idx"]


def _without_time(stats):
    stats = dict(stats)
    stats.pop("updated_at_timestamp_ms")
    return stats


def test_index_stats(tmp_path):
    lance = require_pylance()
    _indexed(lance, str(tmp_path / "p"))
    _indexed(nl, str(tmp_path / "n"))
    for name in INDICES:
        expected = _without_time(lance.dataset(str(tmp_path / "p")).stats.index_stats(name))
        # pylance's own index, read by nanolance: everything the same
        assert _without_time(nl.dataset(str(tmp_path / "p")).stats.index_stats(name)) == expected, name
        # nanolance's index: the same shape and coverage (a vector model trains its own centroids)
        ours = _without_time(nl.dataset(str(tmp_path / "n")).stats.index_stats(name))
        theirs = _without_time(lance.dataset(str(tmp_path / "n")).stats.index_stats(name))
        assert ours == theirs, name
    with pytest.raises(KeyError, match='Index "nope" not found'):
        nl.dataset(str(tmp_path / "p")).stats.index_stats("nope")


def test_dataset_and_data_stats(tmp_path):
    lance = require_pylance()
    for writer in (lance, nl):
        path = str(tmp_path / writer.__name__)
        _indexed(writer, path)
        lance.dataset(path).add_columns({"z": "id * 2"})
        lance.dataset(path).drop_columns(["c"])
        assert nl.dataset(path).stats.dataset_stats() == lance.dataset(path).stats.dataset_stats()
        assert nl.dataset(path).stats.dataset_stats(100) == lance.dataset(path).stats.dataset_stats(100)
        ours = [(f.id, f.bytes_on_disk) for f in nl.dataset(path).stats.data_stats().fields]
        theirs = [(f.id, f.bytes_on_disk) for f in lance.dataset(path).stats.data_stats().fields]
        assert ours == theirs


def test_lance_schema(tmp_path):
    lance = require_pylance()
    from lance.schema import LanceSchema as PySchema

    from nanolance.lance.schema import LanceSchema

    data = pa.table({"x": range(2), "s": [{"a": 1, "b": "hello"}, {"a": 2, "b": "x"}], "y": [[1.0], [2.0]]})
    path = str(tmp_path / "s")
    nl.write_dataset(data, path)
    ours, theirs = nl.dataset(path).lance_schema, lance.dataset(path).lance_schema

    def shape(fields):
        return [(f.name(), f.id(), f.metadata, shape(f.children())) for f in fields]

    assert shape(ours.fields()) == shape(theirs.fields())
    assert repr(ours) == repr(theirs)
    assert ours.to_pyarrow() == theirs.to_pyarrow() == data.schema
    assert pickle.loads(pickle.dumps(ours)) == ours
    assert LanceSchema.from_pyarrow(data.schema) == ours
    assert repr(LanceSchema.from_pyarrow(data.schema)) == repr(PySchema.from_pyarrow(data.schema))
    assert ours.field("s.a").id() == 2 and ours.field("nope") is None
    assert ours.field_case_insensitive("S.A").name() == "a"


def test_update_field_metadata(tmp_path):
    lance = require_pylance()
    data = pa.table({"a": [1], "s": [{"x": 1}]})
    calls = [({"a": {"k1": "v1", "k2": "v2"}}, False), ({"s.x": {"k": "v"}}, False),
             ({"a": {"k1": "new", "k2": None}}, False), ({"a": {"only": "this", "gone": None}}, True)]
    for lib in (nl, lance):
        path = str(tmp_path / lib.__name__)
        lib.write_dataset(data, path)
        for updates, replace in calls:
            lib.dataset(path).update_field_metadata(updates, replace=replace)
    paths = [str(tmp_path / lib.__name__) for lib in (nl, lance)]
    def shape(fields):  # (pylance prints metadata in hash order: compare it as a dict)
        return [(f.name(), f.id(), f.metadata, shape(f.children())) for f in fields]

    for v in range(1, 6):
        schemas = [shape(lib.dataset(p, version=v).lance_schema.fields()) for p in paths for lib in (nl, lance)]
        assert all(s == schemas[0] for s in schemas), v
        assert nl.dataset(paths[0], version=v).schema == lance.dataset(paths[1], version=v).schema
    with pytest.raises(OSError, match="Field 'nope' not found.\nAvailable fields: \\['a', 's', 's.x'\\]"):
        nl.dataset(paths[0]).update_field_metadata({"nope": {"k": "v"}})
    with pytest.raises(TypeError, match="'int' object is not an instance of 'str'"):
        nl.dataset(paths[0]).update_field_metadata({1: {"k": "v"}})


def _norm(x):
    """A transaction as plain data, pylance's and nanolance's classes alike."""
    if type(x).__name__ == "DeletionFile":
        return x.num_deleted_rows
    if type(x).__name__ in ("LanceSchema", "Schema") and hasattr(x, "fields"):
        return [(f.name(), f.id()) for f in x.fields()]
    if dataclasses.is_dataclass(x) and not isinstance(x, type):
        return (type(x).__name__, {f.name.lstrip("_"): _norm(getattr(x, f.name))
                                   for f in dataclasses.fields(x) if f.name != "uuid"})
    if isinstance(x, (list, tuple)):
        return [_norm(v) for v in x]
    if isinstance(x, dict):
        return {k: _norm(v) for k, v in x.items()}
    if isinstance(x, set) or type(x).__name__ == "Bitmap":
        return sorted(x)
    if hasattr(x, "isoformat"):
        return None  # created_at: pylance's is naive local time
    if x is not None and type(x).__name__ == "BaseOperation":
        return "BaseOperation"
    return x


def _history(lib, path):
    t = pa.table({"id": range(100), "s": [f"v{i % 7}" for i in range(100)]})
    lib.write_dataset(t, path, max_rows_per_file=40, commit_message="first", transaction_properties={"k": "v"})
    lib.write_dataset(t, path, mode="append")
    lib.dataset(path).delete("id < 10")
    lib.dataset(path).update({"s": "'x'"}, where="id = 50")
    lib.dataset(path).add_columns({"z": "id * 2"})
    lib.dataset(path).alter_columns({"path": "z", "name": "zz"})
    lib.dataset(path).alter_columns({"path": "s", "nullable": False})
    lib.dataset(path).drop_columns(["zz"])
    lib.dataset(path).update_config({"a": "b"})
    lib.dataset(path).update_field_metadata({"s": {"m": "1"}})
    lib.dataset(path).update_schema_metadata({"sm": "2"})
    lib.dataset(path).create_scalar_index("id", "BTREE")
    lib.dataset(path).merge_insert("id").when_matched_update_all().when_not_matched_insert_all().execute(
        pa.table({"id": [5, 500], "s": ["a", "b"]}))
    lib.dataset(path, version=2).restore()
    lib.write_dataset(t, path, mode="overwrite")
    return lib.dataset(path)


def test_transactions(tmp_path):
    lance = require_pylance()
    ours = _history(nl, str(tmp_path / "n"))
    theirs = _history(lance, str(tmp_path / "p"))
    assert ours.version == theirs.version
    for path in (ours.uri, theirs.uri):
        for v in range(1, ours.version + 1):
            a = _norm(lance.dataset(path).read_transaction(v))
            b = _norm(nl.dataset(path).read_transaction(v))
            assert a == b, (path, v)
    for v in range(1, ours.version + 1):
        kinds = [type(lib.dataset(p).read_transaction(v).operation).__name__
                 for p in (ours.uri, theirs.uri) for lib in (nl, lance)]
        assert len(set(kinds)) == 1, (v, kinds)
    got = nl.dataset(ours.uri).get_transactions(3)
    assert [t.read_version for t in got] == [ours.version - 1, ours.version - 2, ours.version - 3]
    first = nl.dataset(ours.uri).read_transaction(1)
    assert first.transaction_properties == {"__lance_commit_message": "first", "k": "v"}
    assert nl.dataset(ours.uri).read_transaction(2).transaction_properties == {}


def test_merge(tmp_path):
    lance = require_pylance()
    right = pa.table({"a2": [3, 1, 99, 1], "d": ["x", "y", "z", "w"], "e": [1.5, 2.5, 3.5, 4.5]})
    for lib in (nl, lance):
        path = str(tmp_path / lib.__name__)
        lib.write_dataset(pa.table({"a": range(10), "b": range(10)}), path, max_rows_per_file=4)
        lib.dataset(path).merge(right, left_on="a", right_on="a2")
        lib.dataset(path).merge(lib.write_dataset(pa.table({"a": [2], "c": ["two"]}), path + "_r"), "a")
    tables = [lib.dataset(str(tmp_path / w.__name__)).to_table() for w in (nl, lance) for lib in (nl, lance)]
    assert all(t.equals(tables[0]) for t in tables)
    path = str(tmp_path / nl.__name__)
    assert type(lance.dataset(path).read_transaction(2).operation).__name__ == "Merge"
    with pytest.raises(OSError, match="Column nope does not exist in the left side dataset"):
        nl.dataset(path).merge(right, left_on="nope", right_on="a2")
    with pytest.raises(OSError, match="Column d exists in both sides of the dataset"):
        nl.dataset(path).merge(right, left_on="a", right_on="a2")


def test_validate(tmp_path):
    lance = require_pylance()
    ds = _history(nl, str(tmp_path / "v"))
    ds.validate()
    lance.dataset(ds.uri).validate()
    _indexed(nl, str(tmp_path / "i")).validate()
