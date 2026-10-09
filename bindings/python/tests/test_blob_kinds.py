"""Every kind of Lance blob, against pylance: Blob v2 written by either library (inline, packed and
dedicated by size, external by reference, nulls, empties) and read by both; legacy (v1) blob columns
of formats 2.0 and 2.1 read; the write-side checks, errors and options; and the dataset operations
that rewrite blob rows.

Each test does the same thing with both libraries, or reads one library's dataset with both, and
compares what they give: descriptions, bytes, file handles, the stored files and the errors.
"""

from __future__ import annotations

import os
import random
import re

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


@pytest.fixture
def external(tmp_path):
    path = tmp_path / "external.bin"
    path.write_bytes(bytes(range(256)) * 4)
    return path.as_uri()


def _values(lance, uri):
    return [b"tiny", b"p" * 100, b"d" * 5000, lance.blob.Blob.from_uri(uri, position=3, size=10), lance.blob.Blob.empty(),
            b"q" * 200, None, lance.blob.Blob.from_uri(uri)]


def _table(lance, values, **thresholds):
    field = lance.blob_field("blob", **thresholds)
    return pa.table({"id": range(len(values)), "blob": lance.blob_array(values)},
                    schema=pa.schema([pa.field("id", pa.int64()), field]))


def _reads(lib, path, rows):
    ds = lib.dataset(path)
    return (ds.to_table()["blob"].to_pylist(),
            ds.to_table(blob_handling="all_binary")["blob"].to_pylist(),
            [None if f is None else f.readall() for f in ds.take_blobs("blob", indices=list(range(rows)))])


def _sidecars(path):
    return sorted(f for _, _, files in os.walk(os.path.join(path, "data")) for f in files if f.endswith(".blob"))


def test_every_kind_written_and_read_both_ways(tmp_path, external):
    lance = require_pylance()
    values = _values(lance, external)
    table = _table(lance, values, inline_size_threshold=16, dedicated_size_threshold=1000)
    paths = {}
    for lib in (nl, lance):
        paths[lib] = str(tmp_path / lib.__name__)
        lib.write_dataset(table, paths[lib], allow_external_blob_outside_bases=True)
    reads = {(w, r): _reads(r, paths[w], len(values)) for w in (nl, lance) for r in (nl, lance)}
    first = reads[(lance, lance)]
    assert all(read == first for read in reads.values())
    descriptions, data, handles = first
    assert [d and d["kind"] for d in descriptions] == [0, 1, 2, 3, 0, 1, None, 3]
    assert data[:3] == [b"tiny", b"p" * 100, b"d" * 5000] and data[6] is None
    # The same files: one packed sidecar, one dedicated, named by blob id.
    assert _sidecars(paths[nl]) == _sidecars(paths[lance]) == [
        "01000000000000000000000000000000.blob", "10000000000000000000000000000000.blob"]
    # The dataset schema Lance keeps: the logical children and the thresholds.
    assert lance.dataset(paths[nl]).lance_schema == lance.dataset(paths[lance]).lance_schema
    assert nl.dataset(paths[nl]).schema == lance.dataset(paths[lance]).schema


@pytest.mark.parametrize("thresholds,kwargs", [
    ({}, {}),
    ({"inline_size_threshold": 0}, {}),
    ({"inline_size_threshold": 10}, {"blob_pack_file_size_threshold": 150}),
    ({"inline_size_threshold": 10, "pack_file_size_threshold": 150}, {}),
])
def test_placement_by_size_matches(tmp_path, thresholds, kwargs):
    lance = require_pylance()
    rng = random.Random(len(thresholds) + len(kwargs))
    values = [None if rng.random() < 0.1 else bytes([i % 251]) * rng.choice([0, 1, 9, 100, 40, 65536, 65537, 300000])
              for i in range(40)] + [b"x" * (4 * 1024 * 1024), b"y" * (4 * 1024 * 1024 + 1)]
    table = _table(lance, values, **thresholds)
    out = []
    for lib in (nl, lance):
        path = str(tmp_path / lib.__name__)
        lib.write_dataset(table, path, **kwargs)
        out.append((_reads(lance, path, len(values)), _reads(nl, path, len(values)), _sidecars(path)))
    assert out[0] == out[1]
    assert out[0][0][1] == values


def _raw(lance, rows):
    storage = lance.blob.BlobType().storage_type
    return pa.ExtensionArray.from_storage(lance.blob.BlobType(), pa.array(rows, storage))


def _errors(lance, uri):
    row = {"data": None, "uri": uri, "position": None, "size": None}
    return {
        "outside": (lambda: _table(lance, [uri]), {}),
        "both": (lambda: pa.table({"blob": _raw(lance, [{**row, "data": b"x"}])}), {}),
        "neither": (lambda: pa.table({"blob": _raw(lance, [{**row, "uri": None}])}), {}),
        "position_only": (lambda: pa.table({"blob": _raw(lance, [{**row, "position": 1}])}),
                          {"allow_external_blob_outside_bases": True}),
        "range_without_uri": (lambda: pa.table({"blob": _raw(lance, [{**row, "uri": None, "data": b"x", "position": 1,
                                                                      "size": 2}])}),
                              {"allow_external_blob_outside_bases": True}),
        "zero_size": (lambda: pa.table({"blob": _raw(lance, [{**row, "position": 1, "size": 0}])}),
                      {"allow_external_blob_outside_bases": True}),
        "relative": (lambda: _table(lance, ["relative/path.bin"]), {"allow_external_blob_outside_bases": True}),
        "ingest_and_allow": (lambda: _table(lance, [uri]),
                             {"external_blob_mode": "ingest", "allow_external_blob_outside_bases": True}),
        "bad_mode": (lambda: _table(lance, [b"x"]), {"external_blob_mode": "zzz"}),
        "null_in_non_nullable": (lambda: pa.table({"blob": lance.blob_array([None])},
                                                  schema=pa.schema([lance.blob_field("blob", nullable=False)])), {}),
        "bad_threshold": (lambda: pa.table({"blob": lance.blob_array([b"a"])}, schema=pa.schema([
            lance.blob_field("blob").with_metadata({b"ARROW:extension:name": b"lance.blob.v2",
                                                    b"lance-encoding:blob-inline-size-threshold": b"x"})])), {}),
        "zero_threshold": (lambda: pa.table({"blob": lance.blob_array([b"a"])}, schema=pa.schema([
            lance.blob_field("blob").with_metadata({b"ARROW:extension:name": b"lance.blob.v2",
                                                    b"lance-encoding:blob-dedicated-size-threshold": b"0"})])), {}),
    }


@pytest.mark.parametrize("case", ["outside", "both", "neither", "position_only", "range_without_uri", "zero_size",
                                  "relative", "ingest_and_allow", "bad_mode", "null_in_non_nullable",
                                  "bad_threshold", "zero_threshold"])
def test_refused_as_pylance_refuses(tmp_path, external, case):
    lance = require_pylance()
    make, kwargs = _errors(lance, external)[case]
    raised = []
    for lib in (nl, lance):
        with pytest.raises(Exception) as info:
            lib.write_dataset(make(), str(tmp_path / lib.__name__), **kwargs)
        raised.append((type(info.value), re.sub(r", /home/runner.*", "", str(info.value))))
    assert raised[0] == raised[1]


def test_ingest_copies_external_bytes(tmp_path, external):
    lance = require_pylance()
    Blob = lance.blob.Blob
    table = _table(lance, [external, Blob.from_uri(external, 2, 5), b"z" * 70000, None])
    out = []
    for lib in (nl, lance):
        path = str(tmp_path / lib.__name__)
        lib.write_dataset(table, path, external_blob_mode="ingest")
        out.append((_reads(lance, path, 4), _reads(nl, path, 4)))
    assert out[0] == out[1]
    assert out[0][0][1][:2] == [bytes(range(256)) * 4, bytes(range(2, 7))]


def _rows(lance, lo, n, uri):
    rng = random.Random(lo)
    Blob = lance.blob.Blob

    def value(i):
        k = rng.random()
        return (None if k < 0.1 else b"%d" % i * rng.randint(0, 30) if k < 0.6
                else bytes([i % 251]) * rng.randint(100, 3000) if k < 0.9
                else Blob.from_uri(uri, 1, 5) if k < 0.95 else Blob.from_uri(uri))

    schema = pa.schema([pa.field("id", pa.int64()),
                        lance.blob_field("a", inline_size_threshold=64, dedicated_size_threshold=2000),
                        pa.field("s", pa.string()), lance.blob_field("b")])
    return pa.table({"id": range(lo, lo + n), "a": lance.blob_array([value(i) for i in range(n)]),
                     "s": [f"s{i}" for i in range(lo, lo + n)], "b": lance.blob_array([value(i) for i in range(n)])},
                    schema=schema)


def _bytes_only(lance, table):
    """A merge-insert source holds no external URIs: Lance has no way to allow them there."""
    def fix(column):
        return lance.blob_array([None if v is None else v["data"] if v["data"] is not None else b"ext"
                                 for v in column.to_pylist()])
    return table.set_column(1, table.schema.field(1), fix(table["a"])).set_column(3, table.schema.field(3),
                                                                                  fix(table["b"]))


def test_dataset_operations_on_two_blob_columns(tmp_path, external):
    lance = require_pylance()
    steps = [
        lambda lib, p: lib.write_dataset(_rows(lance, 0, 300, external), p, max_rows_per_file=120,
                                         allow_external_blob_outside_bases=True),
        lambda lib, p: lib.write_dataset(_rows(lance, 300, 50, external), p, mode="append",
                                         allow_external_blob_outside_bases=True),
        lambda lib, p: lib.dataset(p).delete("id % 7 = 0"),
        lambda lib, p: lib.dataset(p).update({"s": "'u'"}, where="id < 20"),
        lambda lib, p: lib.dataset(p).merge_insert("id").when_matched_update_all().when_not_matched_insert_all()
        .execute(_bytes_only(lance, _rows(lance, 340, 30, external))),
        lambda lib, p: lib.dataset(p).optimize.compact_files(),
    ]
    paths = {lib: str(tmp_path / lib.__name__) for lib in (nl, lance)}
    for step in steps:
        for lib in (nl, lance):
            step(lib, paths[lib])
        tables = [r.dataset(paths[w]).to_table(blob_handling="all_binary").sort_by("id")
                  for w in (nl, lance) for r in (nl, lance)]
        assert all(t.equals(tables[0]) for t in tables)
        for w in (nl, lance):
            rows = list(range(0, nl.dataset(paths[w]).count_rows(), 11))
            handles = [[None if f is None else f.readall() for f in r.dataset(paths[w]).take_blobs("a", indices=rows)]
                       for r in (nl, lance)]
            assert handles[0] == handles[1]
    with pytest.raises(OSError, match="outside registered external bases"):
        nl.dataset(paths[nl]).merge_insert("id").when_not_matched_insert_all().execute(_rows(lance, 900, 30, external))


def test_append_rules(tmp_path):
    lance = require_pylance()
    storage = pa.struct([pa.field("data", pa.large_binary()), pa.field("uri", pa.utf8())])
    minimal = pa.ExtensionArray.from_storage(lance.blob.BlobType._from_storage_type(storage),
                                             pa.array([{"data": b"m", "uri": None}], storage))
    for lib in (nl, lance):
        path = str(tmp_path / lib.__name__)
        lib.write_dataset(pa.table({"blob": lance.blob_array([b"first"])}), path)
        lib.write_dataset(pa.table({"blob": minimal}), path, mode="append")  # the other logical shape
        assert [f.readall() for f in lib.dataset(path).take_blobs("blob", indices=[0, 1])] == [b"first", b"m"]
        mismatch = pa.table({"blob": lance.blob_array([b"x"])},
                            schema=pa.schema([lance.blob_field("blob", inline_size_threshold=7)]))
        with pytest.raises(OSError, match="Cannot append data with blob threshold metadata "
                                          "lance-encoding:blob-inline-size-threshold=7 for field 'blob'; the dataset "
                                          "schema has effective value 65536"):
            lib.write_dataset(mismatch, path, mode="append")


def test_whole_external_object(tmp_path, external):
    """An external blob without a range is its whole object, sized when read."""
    lance = require_pylance()
    for writer in (nl, lance):
        path = str(tmp_path / writer.__name__)
        writer.write_dataset(_table(lance, [external]), path, allow_external_blob_outside_bases=True)
        for reader in (nl, lance):
            ds = reader.dataset(path)
            assert ds.take_blobs("blob", indices=[0])[0].size() == 1024
            assert ds.to_table(blob_handling="all_binary")["blob"][0].as_py() == bytes(range(256)) * 4


def test_read_blob_ranges_and_pandas(tmp_path, external):
    lance = require_pylance()
    values = [b"in", b"packed!!", b"dedicated payload", external, None, b""]
    schema = pa.schema([lance.blob_field("blobs", inline_size_threshold=4, dedicated_size_threshold=12),
                        pa.field("idx", pa.uint64())])
    table = pa.table({"blobs": lance.blob_array(values), "idx": range(len(values))}, schema=schema)
    path = str(tmp_path / "d")
    lance.write_dataset(table, path, allow_external_blob_outside_bases=True, max_rows_per_file=2)
    requests = [(3, 1, 3), (1, 2, 4), (1, 0, 4), (0, 1, 1), (5, 0, 0), (4, 0, 0), (2, 0, 9)]
    assert nl.dataset(path).read_blob_ranges("blobs", requests, selector="indices") == \
        lance.dataset(path).read_blob_ranges("blobs", requests, selector="indices")
    for bad, message in (([(0, 2**64 - 1, 2)], "offset \\+ length overflowed"), ([(0, 0, 4)], "exceeds blob size")):
        with pytest.raises(ValueError, match=message):
            nl.dataset(path).read_blob_ranges("blobs", bad, selector="indices")
    with pytest.raises(ValueError, match="selector must be one of"):
        nl.dataset(path).read_blob_ranges("blobs", [], selector="offsets")
    pytest.importorskip("pandas")
    for mode in ("lazy", "bytes", "descriptions"):
        ours = nl.dataset(path).to_pandas(blob_mode=mode, filter="idx > 0")
        theirs = lance.dataset(path).to_pandas(blob_mode=mode, filter="idx > 0")
        if mode == "lazy":
            ours["blobs"] = [None if f is None else f.readall() for f in ours["blobs"]]
            theirs["blobs"] = [None if f is None else f.readall() for f in theirs["blobs"]]
        assert ours.to_dict("list") == theirs.to_dict("list")


def test_all_null_blob_column_added_later(tmp_path):
    lance = require_pylance()
    path = str(tmp_path / "d")
    lance.write_dataset(pa.table({"id": range(4)}), path)
    lance.dataset(path).add_columns(lance.blob_field("blob"))
    ds = nl.dataset(path)
    assert ds.to_table(columns=["blob"])["blob"].to_pylist() == [None] * 4
    assert ds.take_blobs("blob", indices=range(4)) == [None] * 4
    assert [d for _, _, d in ds.read_blob_ranges("blob", [(i, 0, 1) for i in range(4)], selector="indices")] == \
        [None] * 4


@pytest.mark.parametrize("version", ["2.0", "2.1"])
def test_legacy_blob_columns_read(tmp_path, version):
    """Blob v1 columns (large_binary marked lance-encoding:blob), which only older formats hold."""
    lance = require_pylance()
    rng = random.Random(1)
    values = [None if rng.random() < 0.1 else bytes([i % 256]) * rng.randint(0, 50) for i in range(20000)]
    schema = pa.schema([pa.field("id", pa.int64()),
                        pa.field("b", pa.large_binary(), metadata={"lance-encoding:blob": "true"})])
    path = str(tmp_path / version)
    lance.write_dataset(pa.table({"id": range(20000), "b": pa.array(values, pa.large_binary())}, schema=schema),
                        path, data_storage_version=version, max_rows_per_file=7000)
    lance.dataset(path).delete("id % 13 = 0")
    ours, theirs = nl.dataset(path), lance.dataset(path)
    assert ours.schema.equals(theirs.schema, check_metadata=True)
    for kwargs in ({}, {"blob_handling": "all_binary"}, {"filter": "id > 15000"}):
        assert ours.to_table(**kwargs).equals(theirs.to_table(**kwargs), check_metadata=True)
    rows = [0, 5, 100, 9999, 15000, 17000]
    assert [None if f is None else f.readall() for f in ours.take_blobs("b", indices=rows)] == \
        [None if f is None else f.readall() for f in theirs.take_blobs("b", indices=rows)]
    assert ours.take(rows).equals(theirs.take(rows))


def test_legacy_blob_write_refused_as_pylance_refuses(tmp_path):
    lance = require_pylance()
    schema = pa.schema([pa.field("b", pa.large_binary(), metadata={"lance-encoding:blob": "true"})])
    for lib in (nl, lance):
        with pytest.raises(OSError, match="Legacy blob columns .* are not supported for file version >= 2.2"):
            lib.write_dataset(pa.table({"b": [b"x"]}, schema=schema), str(tmp_path / lib.__name__))


def _nested_table(lance, n=4000, seed=5):
    """Blobs inside structs (with and without null structs, two deep), lists (null and empty lists)
    and lists of structs, beside a vector: every shape nested blobs come in."""
    rng = random.Random(seed)

    def blob(i):
        k = rng.random()
        return None if k < 0.1 else bytes([i % 251]) * rng.choice([0, 3, 30, 200, 5000])

    def field(name):
        return lance.blob_field(name, inline_size_threshold=16, dedicated_size_threshold=1000)

    types = {
        "s": pa.struct([field("x"), pa.field("y", pa.int64())]),
        "d": pa.struct([pa.field("deep", pa.struct([field("b")]))]),
        "l": pa.list_(field("item")),
        "ls": pa.list_(pa.field("item", pa.struct([field("x"), pa.field("k", pa.string())]))),
    }
    rows = [{"s": None if rng.random() < 0.1 else {"x": blob(i), "y": i},
             "d": None if rng.random() < 0.05 else {"deep": None if rng.random() < 0.1 else {"b": blob(i)}},
             "l": None if rng.random() < 0.1 else [blob(i + j) for j in range(rng.randint(0, 3))],
             "ls": None if rng.random() < 0.1 else [None if rng.random() < 0.1 else {"x": blob(i), "k": f"k{j}"}
                                                     for j in range(rng.randint(0, 2))]} for i in range(n)]

    def storage(t):
        if getattr(t, "extension_name", None) == "lance.blob.v2":
            return t.storage_type
        if pa.types.is_struct(t):
            return pa.struct([pa.field(f.name, storage(f.type)) for f in t])
        if pa.types.is_list(t):
            return pa.list_(pa.field(t.value_field.name, storage(t.value_type)))
        return t

    def logical(v, t):
        if v is None:
            return None
        if getattr(t, "extension_name", None) == "lance.blob.v2":
            return {"data": v, "uri": None, "position": None, "size": None}
        if pa.types.is_struct(t):
            return {f.name: logical(v[f.name], f.type) for f in t}
        if pa.types.is_list(t):
            return [logical(x, t.value_type) for x in v]
        return v

    def extension(arr, t):
        if getattr(t, "extension_name", None) == "lance.blob.v2":
            return pa.ExtensionArray.from_storage(t, arr)
        if pa.types.is_struct(t):
            return pa.StructArray.from_arrays([extension(arr.field(k), t.field(k).type) for k in range(t.num_fields)],
                                              fields=list(t), mask=arr.is_null())
        if pa.types.is_list(t):
            return pa.ListArray.from_arrays(arr.offsets, extension(arr.values, t.value_type),
                                            type=pa.list_(t.value_field), mask=arr.is_null())
        return arr

    columns = {"id": pa.array(range(n), pa.int64())}
    for name, t in types.items():
        columns[name] = extension(pa.array([logical(r[name], t) for r in rows], storage(t)), t)
    columns["v"] = pa.array([[float(i), 1.0] for i in range(n)], pa.list_(pa.float32(), 2))
    schema = pa.schema([pa.field("id", pa.int64())] + [pa.field(k, t) for k, t in types.items()] +
                       [pa.field("v", pa.list_(pa.float32(), 2))])
    return pa.Table.from_arrays([columns[f.name] for f in schema], schema=schema)


def test_nested_blobs_written_and_read_both_ways(tmp_path):
    """Blobs inside structs and lists, written by either library -- as Lance pages them, with the
    structs' definition and the lists' repetition levels -- and read by both, deletions included."""
    lance = require_pylance()
    table = _nested_table(lance)
    for writer in (nl, lance):
        path = str(tmp_path / writer.__name__)
        writer.write_dataset(table, path, max_rows_per_file=1500)
        writer.dataset(path).delete("id % 17 = 0")
        for kwargs in ({}, {"blob_handling": "all_binary"}, {"filter": "id > 2000 and id < 2300"},
                       {"columns": ["ls", "id"], "blob_handling": "all_binary"}):
            assert nl.dataset(path).to_table(**kwargs).equals(lance.dataset(path).to_table(**kwargs)), \
                (writer.__name__, kwargs)
        rows = [0, 5, 100, 1600, 3999 - 3999 // 17 - 1]
        assert nl.dataset(path).take(rows).equals(lance.dataset(path).to_table().take(rows))  # (pylance's own
        # take of nested blobs panics)
        for field_path in ("s.x", "d.deep.b"):
            ours = [None if f is None else f.readall() for f in nl.dataset(path).take_blobs(field_path, indices=rows)]
            theirs = [None if f is None else f.readall() for f in lance.dataset(path).take_blobs(field_path, indices=rows)]
            assert ours == theirs, field_path
        assert nl.dataset(path).read_blobs("s.x", indices=rows) == lance.dataset(path).read_blobs("s.x", indices=rows)
    assert lance.dataset(str(tmp_path / "nanolance.lance")).lance_schema == lance.dataset(str(tmp_path / "lance")).lance_schema


def test_nested_blob_datasets_rewritten(tmp_path):
    lance = require_pylance()
    table = _nested_table(lance, n=2500, seed=9)
    for lib in (nl, lance):
        path = str(tmp_path / lib.__name__)
        lib.write_dataset(table, path, max_rows_per_file=1000)
        lib.dataset(path).delete("id % 13 = 0")
        lib.dataset(path).update({"id": "id + 0"}, where="id < 100")
        lib.dataset(path).optimize.compact_files(target_rows_per_fragment=10000)
        lib.write_dataset(table.slice(0, 50), path, mode="append")
    tables = [r.dataset(str(tmp_path / w.__name__)).to_table(blob_handling="all_binary").sort_by("id")
              for w in (nl, lance) for r in (nl, lance)]
    assert all(t.equals(tables[0]) for t in tables)


def test_blob_beside_vectors_lists_and_structs(tmp_path):
    """A blob column in the same read as a fixed-size list, a list and a struct (it used to refuse)."""
    lance = require_pylance()
    table = pa.table({"vec": pa.FixedSizeListArray.from_arrays(pa.array([0.5] * 8, pa.float32()), 4),
                      "img": lance.blob_array([b"a", None]), "tags": [["x"], []], "meta": [{"w": 1}, None]})
    for writer in (nl, lance):
        path = str(tmp_path / writer.__name__)
        writer.write_dataset(table, path)
        for kwargs in ({}, {"blob_handling": "all_binary"}, {"columns": ["vec", "img"]}):
            assert nl.dataset(path).to_table(**kwargs).equals(lance.dataset(path).to_table(**kwargs))


def test_large_blob_page_takes_in_pylance(tmp_path, external):
    """A page of more than 64 KiB of blob descriptors: its row index is Lance's (rows + 1 offsets,
    byte-packed), so pylance takes rows from it -- it panicked on nanolance's former u32 form."""
    lance = require_pylance()
    path = str(tmp_path / "d")
    rows = 5000
    nl.write_dataset(pa.table({"id": range(rows), "b": lance.blob_array([lance.blob.Blob.from_uri(external, 1, 5)] * rows)}),
                     path, allow_external_blob_outside_bases=True)
    assert [r["b"]["size"] for r in lance.dataset(path).take([3, 4000]).to_pylist()] == [5, 5]
    nl.dataset(path).delete("id % 7 = 0")
    assert lance.dataset(path).to_table().equals(nl.dataset(path).to_table())
