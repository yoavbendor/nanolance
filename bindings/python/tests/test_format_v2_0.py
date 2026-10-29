"""Lance file format 2.0, read by nanolance and compared with pylance.

2.0 (footer 0.3) is what LanceDB wrote by default for a long time: 281 of the 621 Lance tables on the
Hugging Face Hub are 2.0, and nearly every LanceDB table among them. Its pages are `ArrayEncoding`
trees (Nullable / Flat / Binary / Dictionary / FixedSizeList / Fsst ...), and every field -- a
struct's and a list's too -- has a column of its own. pylance 12 still writes it with
`data_storage_version="2.0"`, which is what these tests use.
"""

from __future__ import annotations

import glob
import os
import struct
import warnings

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance import file as nl_file
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _table(n: int = 3_000, seed: int = 0) -> pa.Table:
    rng = np.random.default_rng(seed)
    valid = rng.random(n) > 0.1
    words = [f"w{i % 37}" for i in range(n)]
    long_text = [("text %d " % i) * (i % 9) for i in range(n)]
    emb = rng.standard_normal(n * 16, dtype=np.float32)
    elem_nulls = rng.random(n * 16) < 0.02
    return pa.table({
        "id": pa.array(np.arange(n, dtype=np.int64)),
        "i32": pa.array(rng.integers(-1000, 1000, n, dtype=np.int32), mask=~valid),
        "u8": pa.array(rng.integers(0, 255, n, dtype=np.uint8)),
        "f64": pa.array(rng.standard_normal(n), mask=rng.random(n) < 0.05),
        "flag": pa.array(rng.random(n) < 0.4, mask=rng.random(n) < 0.05),
        "name": pa.array([w if v else None for w, v in zip(words, valid)]),  # low cardinality: Dictionary
        "text": pa.array(long_text, pa.large_string()),
        "blob": pa.array([bytes(rng.integers(0, 255, i % 50, dtype=np.uint8)) if i % 13 else None for i in range(n)],
                         pa.binary()),
        "emb": pa.FixedSizeListArray.from_arrays(pa.array(emb, mask=elem_nulls), 16,
                                                 mask=pa.array(rng.random(n) < 0.03)),
        "day": pa.array(rng.integers(0, 20_000, n, dtype=np.int32), pa.date32()),
        "ts": pa.array(rng.integers(0, 2**40, n), pa.timestamp("us")),
        "fsb": pa.array([bytes([i % 256]) * 4 for i in range(n)], pa.binary(4)),
        "nothing": pa.nulls(n),
        "point": pa.StructArray.from_arrays(
            [pa.array(rng.integers(0, 100, n, dtype=np.int16)), pa.array([f"p{i % 5}" for i in range(n)])],
            names=["x", "label"]),
        "tags": pa.array([[f"t{j}" for j in range(i % 4)] if i % 11 else None for i in range(n)],
                         pa.list_(pa.string())),
        "nums": pa.array([list(range(i % 5)) for i in range(n)], pa.large_list(pa.int64())),
        "objs": pa.array([[{"a": i, "b": f"s{i}"}] * (i % 3) for i in range(n)],
                         pa.list_(pa.struct([("a", pa.int64()), ("b", pa.string())]))),
        "nested": pa.array([{"v": list(range(i % 3)), "w": i} for i in range(n)],
                           pa.struct([("v", pa.list_(pa.int32())), ("w", pa.int64())])),
    })


@pytest.fixture(scope="module")
def v20(lance, tmp_path_factory):
    path = tmp_path_factory.mktemp("v20") / "t.lance"
    table = _table()
    lance.write_dataset(table, str(path), data_storage_version="2.0", max_rows_per_file=1_100)
    return str(path), table


def test_pylance_writes_format_2_0(v20):
    path, _ = v20
    for f in glob.glob(f"{path}/data/*.lance"):
        with open(f, "rb") as fh:
            fh.seek(-8, 2)
            assert fh.read(4) == b"\x00\x00\x03\x00"  # footer 0.3


def test_whole_dataset_matches_pylance(lance, v20):
    path, table = v20
    got = nl.dataset(path).to_table()
    assert got.equals(lance.dataset(path).to_table())
    assert got.equals(table)


@pytest.mark.parametrize("offset,limit", [(0, 10), (5, 1_200), (1_099, 3), (1_500, 1_500), (2_999, 1)])
def test_row_ranges(lance, v20, offset, limit):
    path, _ = v20
    assert nl.dataset(path).to_table(offset=offset, limit=limit).equals(
        lance.dataset(path).to_table(offset=offset, limit=limit))


def test_take(lance, v20):
    path, _ = v20
    rng = np.random.default_rng(3)
    idx = sorted(rng.choice(3_000, 400, replace=False).tolist())
    assert nl.dataset(path).take(idx).equals(lance.dataset(path).take(idx))
    run = list(range(1_090, 1_120))  # across a file boundary
    assert nl.dataset(path).take(run).equals(lance.dataset(path).take(run))


def test_projection_and_filter(lance, v20):
    path, _ = v20
    cols = ["name", "emb", "tags", "point"]
    assert nl.dataset(path).to_table(columns=cols).equals(lance.dataset(path).to_table(columns=cols))
    filt = "id % 7 = 3"
    assert nl.dataset(path).to_table(filter=filt).equals(lance.dataset(path).to_table(filter=filt))


def test_deletions(lance, tmp_path):
    path = str(tmp_path / "d.lance")
    lance.write_dataset(_table(800, seed=5), path, data_storage_version="2.0", max_rows_per_file=300)
    lance.dataset(path).delete("id % 5 = 0")
    assert nl.dataset(path).to_table().equals(lance.dataset(path).to_table())
    assert nl.dataset(path).take([0, 1, 100, 500]).equals(lance.dataset(path).take([0, 1, 100, 500]))


def test_data_files_through_the_file_reader(lance, v20):
    path, _ = v20
    from lance.file import LanceFileReader

    for f in sorted(glob.glob(f"{path}/data/*.lance")):
        theirs, ours = LanceFileReader(f), nl_file.LanceFileReader(f)
        assert ours.num_rows() == theirs.num_rows()
        assert ours.read_all().to_table().equals(theirs.read_all().to_table())
        assert ours.read_range(7, 50).to_table().equals(theirs.read_range(7, 50).to_table())
        rows = [0, 3, 4, 5, 200, ours.num_rows() - 1]
        assert ours.take_rows(rows).to_table().equals(theirs.take_rows(rows).to_table())


@pytest.mark.parametrize("scheme", ["fsst", "zstd", "lz4"])
def test_compressed_strings(lance, tmp_path, scheme):
    """2.0 compresses strings only when a field asks: FSST, or zstd/lz4 around the bytes buffer."""
    n = 2_000
    field = pa.field("s", pa.string(), metadata={"lance-encoding:compression": scheme})
    table = pa.table({"s": pa.array([f"value number {i} " * (i % 4) if i % 9 else None for i in range(n)])},
                     schema=pa.schema([field]))
    path = str(tmp_path / f"{scheme}.lance")
    lance.write_dataset(table, path, data_storage_version="2.0")
    assert nl.dataset(path).to_table().column("s").equals(table.column("s"))
    assert nl.dataset(path).take([1, 2, 999, 1_998]).equals(lance.dataset(path).take([1, 2, 999, 1_998]))


def test_blob_column(lance, tmp_path):
    """A 2.0 blob column: (position, size) descriptions in a packed struct, the bytes elsewhere in the
    data file. pylance reads the bytes; so does nanolance."""
    n = 50
    field = pa.field("img", pa.large_binary(), metadata={"lance-encoding:blob": "true"})
    values = [bytes([i % 251]) * (i * 37 % 4000) if i % 7 else None for i in range(n)]
    table = pa.table({"id": pa.array(range(n)), "img": pa.array(values, pa.large_binary())},
                     schema=pa.schema([pa.field("id", pa.int64()), field]))
    path = str(tmp_path / "blob.lance")
    lance.write_dataset(table, path, data_storage_version="2.0")
    from lance.file import LanceFileReader

    (f,) = glob.glob(f"{path}/data/*.lance")
    assert nl_file.LanceFileReader(f).read_all().to_table().equals(LanceFileReader(f).read_all().to_table())
    assert nl_file.LanceFileReader(f).read_all().to_table().column("img").to_pylist() == values


def test_packed_struct(lance, tmp_path):
    """A struct the writer packed into one column (field metadata `packed`): its fields -- fixed-width,
    never null -- have no columns of their own, so each is read out of the struct's column."""
    n = 2_500
    rng = np.random.default_rng(9)
    packed = pa.field("p", pa.struct([("x", pa.int32()), ("y", pa.float64()),
                                      ("v", pa.list_(pa.float32(), 3)), ("t", pa.timestamp("ms"))]),
                      metadata={"packed": "true"})
    p = pa.StructArray.from_arrays(
        [pa.array(rng.integers(-9, 9, n, dtype=np.int32)), pa.array(rng.standard_normal(n)),
         pa.FixedSizeListArray.from_arrays(pa.array(rng.standard_normal(3 * n, dtype=np.float32)), 3),
         pa.array(rng.integers(0, 2**40, n), pa.timestamp("ms"))],
        fields=list(packed.type))
    schema = pa.schema([pa.field("id", pa.int64()), packed, pa.field("z", pa.string())])
    table = pa.table([pa.array(range(n)), p, pa.array([f"z{i}" for i in range(n)])], schema=schema)
    path = str(tmp_path / "packed.lance")
    lance.write_dataset(table, path, data_storage_version="2.0", max_rows_per_file=1_000)
    ours, theirs = nl.dataset(path), lance.dataset(path)
    assert ours.to_table().equals(theirs.to_table())
    assert ours.to_table().equals(table)
    assert ours.to_table(offset=990, limit=30).equals(theirs.to_table(offset=990, limit=30))
    rows = [0, 5, 6, 7, 999, 1_000, 2_499]
    assert ours.take(rows).equals(theirs.take(rows))
    assert ours.to_table(columns=["p"]).equals(theirs.to_table(columns=["p"]))
    from lance.file import LanceFileReader

    for f in sorted(glob.glob(f"{path}/data/*.lance")):
        assert nl_file.LanceFileReader(f).read_all().to_table().equals(LanceFileReader(f).read_all().to_table())


@pytest.mark.parametrize("version", ["2.0", "2.1"])
def test_append_to_an_older_format(lance, tmp_path, version):
    """nanolance writes 2.2 files. Appended to a 2.0 or 2.1 dataset -- most of the Hub's -- they made
    it unreadable by pylance ("mixed data-file-version capability is not enabled"); Lance's own
    commits set that capability (a manifest feature flag) when they mix versions, and nanolance now
    does too. And rows read back from a 2.0 table with lists or structs would not append at all:
    format 2.0 gives a list its own column, and nanolance compared that with a batch's schema."""
    path = str(tmp_path / "t.lance")
    n = 400
    rng = np.random.default_rng(2)
    first = pa.table({
        "id": pa.array(np.arange(n, dtype=np.int64)),
        "name": pa.array([f"n{i}" if i % 9 else None for i in range(n)]),
        "tags": pa.array([[f"t{j}" for j in range(i % 4)] if i % 11 else None for i in range(n)],
                         pa.list_(pa.string())),
        "point": pa.StructArray.from_arrays([pa.array(rng.integers(0, 100, n, dtype=np.int16)),
                                             pa.array([f"p{i % 5}" for i in range(n)])], names=["x", "label"]),
        "objs": pa.array([[{"a": i, "b": f"s{i}"}] * (i % 3) for i in range(n)],
                         pa.list_(pa.struct([("a", pa.int64()), ("b", pa.string())]))),
        "emb": pa.FixedSizeListArray.from_arrays(pa.array(rng.standard_normal(n * 8, dtype=np.float32)), 8),
    })
    lance.write_dataset(first, path, data_storage_version=version, max_rows_per_file=200)
    more = nl.dataset(path).to_table(offset=10, limit=50)  # what a user appends: rows like the others
    with warnings.catch_warnings():  # the first 2.2 file beside 2.0 / 2.1 ones: test_mixed_format_warning.py
        warnings.simplefilter("ignore", nl.MixedFormatWarning)
        nl.write_dataset(more, path, mode="append")
    ds = lance.dataset(path)
    ds.validate()
    assert ds.count_rows() == 450
    assert ds.to_table().equals(pa.concat_tables([first, more]))
    assert nl.dataset(path).to_table().equals(ds.to_table())


N_ROWS = 300
MODIFY_OPS = ["append", "delete", "update", "merge_insert", "add_columns", "drop_columns", "compact", "overwrite"]
INDEX_OPS = ["btree", "inverted", "ivf_pq"]
FOOTER_OF = {(2, 0): (0, 3)}


def _rows(ids, tag: str = "row") -> pa.Table:
    """int64 id, a string, an 8-float vector (deterministic in the id) and a list<int64>."""
    ids = np.asarray(ids, dtype=np.int64)
    vec = np.sin(np.outer(ids, np.arange(1, 9)) * 0.37).astype(np.float32).ravel()
    return pa.table({
        "id": pa.array(ids),
        "s": pa.array([f"{tag} {i} word{i % 7}" for i in ids.tolist()]),
        "vec": pa.FixedSizeListArray.from_arrays(pa.array(vec), 8),
        "tags": pa.array([[i] * (i % 3) for i in ids.tolist()], pa.list_(pa.int64())),
    })


def _apply(lib, path: str, op: str) -> None:
    """One operation, run by `lib` (nanolance or pylance) on the dataset at `path`."""
    if op == "append":
        lib.write_dataset(_rows(range(N_ROWS, N_ROWS + 60), "late"), path, mode="append")
    elif op == "delete":
        lib.dataset(path).delete("id % 5 = 0")
    elif op == "update":
        lib.dataset(path).update({"s": "'updated'"}, where="id < 40")
    elif op == "merge_insert":
        (lib.dataset(path).merge_insert("id").when_matched_update_all().when_not_matched_insert_all()
         .execute(_rows(range(250, 350), "merged")))
    elif op == "add_columns":
        lib.dataset(path).add_columns({"extra": "id * 2"})
    elif op == "drop_columns":
        lib.dataset(path).drop_columns(["tags"])
    elif op == "compact":
        lib.dataset(path).delete("id % 5 = 0")
        lib.dataset(path).optimize.compact_files()
    elif op == "overwrite":
        lib.write_dataset(_rows(range(1_000, 1_200), "new"), path, mode="overwrite")
    elif op == "btree":
        lib.dataset(path).create_scalar_index("id", index_type="BTREE")
    elif op == "inverted":
        lib.dataset(path).create_scalar_index("s", index_type="INVERTED")
    elif op == "ivf_pq":
        lib.dataset(path).create_index("vec", index_type="IVF_PQ", num_partitions=4, num_sub_vectors=4)
    else:
        raise ValueError(op)


def _footer_version(path: str) -> tuple:
    with open(path, "rb") as fh:
        fh.seek(-8, 2)
        return struct.unpack("<HH", fh.read(4))


def _manifest_files(ds) -> dict:
    """{data file name: (major, minor)} for the data files the dataset's current version references."""
    out = {}
    for frag in ds.get_fragments():
        for f in frag.data_files():
            out[os.path.basename(f.path)] = (f.file_major_version, f.file_minor_version)
    return out


def _query_ids(lib, path: str, op: str) -> list:
    ds = lib.dataset(path)
    if op == "btree":
        return sorted(ds.to_table(filter="id >= 40 and id < 90", columns=["id"]).column("id").to_pylist())
    if op == "inverted":
        return sorted(ds.to_table(full_text_query="word3", columns=["id"]).column("id").to_pylist())
    q = _rows([17]).column("vec")[0].as_py()
    return ds.to_table(nearest={"column": "vec", "q": q, "k": 10, "nprobes": 4},
                       columns=["id"]).column("id").to_pylist()


@pytest.mark.parametrize("version", ["2.0", "2.1"])
@pytest.mark.parametrize("op", MODIFY_OPS + INDEX_OPS)
def test_operation_on_an_older_dataset(lance, tmp_path, version, op):
    """Every modification of a 2.0 / 2.1 dataset: nanolance and pylance each run it on their own copy.
    Results are compared (values sorted by id; for index builds, query results from both readers),
    then pylance appends to each copy. Files nanolance writes are 2.2; files it leaves keep theirs."""
    paths = {"nl": str(tmp_path / "nl.lance"), "pl": str(tmp_path / "pl.lance")}
    for path in paths.values():
        lance.write_dataset(_rows(range(N_ROWS)), path, data_storage_version=version, max_rows_per_file=100)
    before = {k: _manifest_files(lance.dataset(p)) for k, p in paths.items()}
    with warnings.catch_warnings():  # the mixed-format warning is checked in test_mixed_format_warning.py
        warnings.simplefilter("ignore", nl.MixedFormatWarning)
        _apply(nl, paths["nl"], op)
    _apply(lance, paths["pl"], op)

    ours = lance.dataset(paths["nl"])
    assert ours.data_storage_version == ("2.2" if op == "overwrite" else version)

    after = _manifest_files(ours)
    for name, ver in after.items():
        if name in before["nl"]:
            assert ver == before["nl"][name], f"{name} changed version"
        else:
            assert ver == (2, 2), f"new data file {name} is {ver}, not 2.2"
    if op in ("append", "update", "merge_insert", "add_columns", "compact", "overwrite"):
        assert set(after) - set(before["nl"]), "this operation writes data files"
    for name in after:  # the footer spells format 2.0 as 0.3 (the manifest says 2.0)
        assert _footer_version(os.path.join(paths["nl"], "data", name)) == FOOTER_OF.get(after[name], after[name])

    if op in INDEX_OPS:
        for key in paths:
            assert lance.dataset(paths[key]).describe_indices(), f"no index on the {key} copy"
            assert _query_ids(lance, paths[key], op)
            assert _query_ids(nl, paths[key], op) == _query_ids(lance, paths[key], op), key
    else:
        assert ours.to_table().sort_by("id").equals(lance.dataset(paths["pl"]).to_table().sort_by("id"))

    # pylance appends to each copy, using the schema the result now has
    extra = lance.dataset(paths["pl"]).to_table().slice(0, 20)
    extra = extra.set_column(extra.schema.get_field_index("id"), "id", pa.array(range(10_000, 10_020), pa.int64()))
    for path in paths.values():
        lance.write_dataset(extra, path, mode="append")
    assert lance.dataset(paths["nl"]).count_rows() == lance.dataset(paths["pl"]).count_rows()
