"""nanolance datasets in the engines people read Lance with -- DuckDB, Polars, pandas and LanceDB --
compared with pylance and with each engine over the same data in memory.

* A ``nanolance.lance`` dataset is a ``pyarrow.dataset.Dataset`` (as pylance's is), so DuckDB's
  replacement scans and ``duckdb.from_arrow``, and Polars' ``scan_pyarrow_dataset`` /
  ``to_polars()``, read it with projections and filters pushed down.
* The files nanolance writes are read by other Lance implementations: DuckDB's ``lance`` extension
  and LanceDB (scans, filters, vector and full-text search over indexes nanolance built), and
  nanolance reads and appends to tables LanceDB writes.

The DuckDB extension and LanceDB are optional (``tools/interop_suite.py`` installs them in their
own environments and runs this file there); without them those tests skip.
"""

from __future__ import annotations

import datetime as dt
import pickle

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

duckdb = pytest.importorskip("duckdb")
pl = pytest.importorskip("polars")


def _table(n=3000, seed=0):
    rng = np.random.default_rng(seed)
    return pa.table({
        "id": range(n),
        "s": [None if i % 11 == 0 else f"x{i % 7}" for i in range(n)],
        "f": rng.random(n),
        "b": [i % 2 == 0 for i in range(n)],
        "d": [dt.date(2020, 1, 1) + dt.timedelta(days=i) for i in range(n)],
        "ts": [dt.datetime(2020, 1, 1) + dt.timedelta(hours=i) for i in range(n)],
        "text": [f"hello world doc{i} {'apple' if i % 5 == 0 else 'pear'}" for i in range(n)],
        "l": [[i, i + 1] for i in range(n)],
        "n": [None if i % 3 == 0 else i for i in range(n)],
        "vector": pa.FixedSizeListArray.from_arrays(pa.array(rng.random(n * 16).astype("float32")), 16),
    })


@pytest.fixture(scope="module")
def datasets(tmp_path_factory):
    """The same table written by nanolance and by pylance, several files each."""
    lance = require_pylance()
    root = tmp_path_factory.mktemp("interop")
    table = _table()
    paths = {}
    for lib in (nl, lance):
        paths[lib.__name__] = str(root / f"{lib.__name__}.lance")
        lib.write_dataset(table, paths[lib.__name__], max_rows_per_file=1000)
    return table, paths


def test_is_a_pyarrow_dataset(datasets):
    _, paths = datasets
    ds = nl.dataset(paths["nanolance.lance"])
    assert isinstance(ds, pa.dataset.Dataset)
    assert isinstance(ds.scanner(), pa.dataset.Scanner)
    assert isinstance(ds.get_fragments()[0], pa.dataset.Fragment)
    assert pickle.loads(pickle.dumps(ds)).count_rows() == 3000
    for call in (lambda: ds.partition_expression, lambda: ds.replace_schema(ds.schema),
                 lambda: ds.join(ds, "id"), lambda: ds.get_fragments()[0].physical_schema):
        with pytest.raises(NotImplementedError):
            call()


DUCKDB_QUERIES = [
    "select count(*), round(sum(f), 9), count(n), count(s), min(d), max(ts) from {t}",
    "select id, s from {t} where id between 100 and 140 and s <> 'x1' order by id",
    "select s, count(*), round(avg(f), 9) from {t} where f > 0.5 group by s order by s nulls first",
    "select id from {t} where s is null and b order by id limit 7",
    "select id from {t} where s in ('x2', 'x4') and d >= date '2026-01-01' order by id",
    "select id from {t} where ts < timestamp '2020-01-02 03:00:00' order by id",
    "select id, l[2] from {t} where text like '%apple%' and id < 60 order by id",
    "select count(*) from {t} where n is not null and not b",
]


@pytest.mark.parametrize("query", DUCKDB_QUERIES)
def test_duckdb_replacement_scan(datasets, query):
    """DuckDB over a nanolance dataset, a pylance dataset, and the table in memory: one answer."""
    lance = require_pylance()
    table, paths = datasets
    memory = table  # noqa: F841 -- read by DuckDB by name
    expected = duckdb.sql(query.format(t="memory")).fetchall()
    for lib in (nl, lance):
        for path in paths.values():
            ds = lib.dataset(path)  # noqa: F841 -- read by DuckDB by name
            assert duckdb.sql(query.format(t="ds")).fetchall() == expected, (lib.__name__, path)


def test_duckdb_relations(datasets):
    table, paths = datasets
    ds = nl.dataset(paths["nanolance.lance"])
    assert duckdb.from_arrow(ds).filter("id < 3").project("id, s").fetchall() == [(0, None), (1, "x1"), (2, "x2")]
    scanner = ds.scanner(columns=["id", "f"], filter="id < 10")  # noqa: F841 -- a scanner is an Arrow source too
    assert duckdb.sql("select count(*), max(id) from scanner").fetchall() == [(10, 9)]
    con = duckdb.connect()
    con.register("t", ds)
    assert con.sql("select count(*) from t where b").fetchall() == [(1500,)]


POLARS_PREDICATES = {
    "gt": lambda: pl.col("id") > 2900,
    "is_in": lambda: pl.col("s").is_in(["x1", "x3"]),
    "is_null": lambda: pl.col("s").is_null(),
    "and": lambda: pl.col("s").is_not_null() & (pl.col("f") < 0.1),
    "eq_str": lambda: pl.col("s") == "x2",
    "bool": lambda: pl.col("b"),
    "not": lambda: ~pl.col("b"),
    "date": lambda: pl.col("d") > dt.date(2027, 1, 1),
    "timestamp": lambda: pl.col("ts") < dt.datetime(2020, 1, 3),
    "between": lambda: pl.col("f").is_between(0.25, 0.26),
    "or": lambda: (pl.col("id") < 3) | (pl.col("id") > 2997),
    "starts_with": lambda: pl.col("s").str.starts_with("x6"),
}


@pytest.mark.parametrize("name", sorted(POLARS_PREDICATES))
def test_polars_scan(datasets, name):
    """Polars' filters, pushed into the scan, select the rows Polars selects in memory."""
    lance = require_pylance()
    table, paths = datasets
    predicate = POLARS_PREDICATES[name]()
    columns = ["id", "s", "f", "d"]
    expected = pl.from_arrow(table).filter(predicate).select(columns).sort("id")
    for path in paths.values():
        for frame in (nl.dataset(path).to_polars(), pl.scan_pyarrow_dataset(nl.dataset(path)),
                      pl.scan_pyarrow_dataset(lance.dataset(path))):
            assert frame.filter(predicate).select(columns).collect().sort("id").equals(expected), path


def test_polars_frames(datasets):
    table, paths = datasets
    ds = nl.dataset(paths["nanolance.lance"])
    frame = ds.to_polars(batch_size=128)
    assert frame.select(pl.len()).collect().item() == 3000
    sums = frame.group_by("s").agg(pl.col("f").sum()).sort("s", nulls_last=False).collect()
    expected = pl.from_arrow(table).group_by("s").agg(pl.col("f").sum()).sort("s", nulls_last=False)
    assert sums["s"].to_list() == expected["s"].to_list()
    assert np.allclose(sums["f"].to_numpy(), expected["f"].to_numpy())  # (summed in another order)
    assert pl.from_arrow(ds.to_table(columns=["id", "vector"])).shape == (3000, 2)
    assert pl.from_arrow(ds.scanner(filter="id < 5").to_reader().read_all()).height == 5
    # A polars DataFrame written, and read back as one.
    written = pl.DataFrame({"a": [1, 2, 3], "b": ["x", "y", None]})
    nl.write_dataset(written, paths["nanolance.lance"] + "_pl")
    assert nl.dataset(paths["nanolance.lance"] + "_pl").to_polars().collect().equals(written)


def test_pandas_matches_pylance(datasets):
    pd = pytest.importorskip("pandas")
    lance = require_pylance()
    _, paths = datasets
    for path in paths.values():
        ours = nl.dataset(path).to_pandas(columns=["id", "s", "f", "d", "n"], filter="id % 3 = 1")
        theirs = lance.dataset(path).to_pandas(columns=["id", "s", "f", "d", "n"], filter="id % 3 = 1")
        pd.testing.assert_frame_equal(ours, theirs)
    written = pd.DataFrame({"a": [1.5, None], "b": ["x", "y"]})
    nl.write_dataset(written, paths["nanolance.lance"] + "_pd")
    pd.testing.assert_frame_equal(nl.dataset(paths["nanolance.lance"] + "_pd").to_pandas(), written)


# ── other Lance implementations reading nanolance's files ─────────────────────────────────────────


def _duckdb_lance():
    con = duckdb.connect()
    try:
        con.sql("LOAD lance")
    except duckdb.Error:
        try:
            con.sql("INSTALL lance FROM community")
            con.sql("LOAD lance")
        except duckdb.Error as exc:
            pytest.skip(f"DuckDB's lance extension is not available for DuckDB {duckdb.__version__}: {exc}")
    return con


@pytest.fixture(scope="module")
def indexed(tmp_path_factory):
    """A nanolance dataset with nanolance's own IVF_PQ and INVERTED indexes."""
    path = str(tmp_path_factory.mktemp("indexed") / "nano.lance")
    nl.write_dataset(_table(2000), path, max_rows_per_file=700)
    nl.dataset(path).create_index("vector", "IVF_PQ", num_partitions=4, num_sub_vectors=4)
    nl.dataset(path).create_scalar_index("text", "INVERTED")
    nl.dataset(path).create_scalar_index("id", "BTREE")
    return path


def test_duckdb_lance_extension_reads_nanolance(indexed):
    con = _duckdb_lance()
    table = nl.dataset(indexed).to_table()
    memory = table  # noqa: F841
    for query in DUCKDB_QUERIES:
        assert con.sql(query.format(t=f"'{indexed}'")).fetchall() == duckdb.sql(query.format(t="memory")).fetchall()
    vector = "[" + ",".join(repr(float(x)) for x in table["vector"][7].as_py()) + "]::FLOAT[16]"
    exact = con.sql(f"select id from lance_vector_search('{indexed}', 'vector', {vector}, k=3, use_index=false) "
                    "order by _distance limit 1").fetchall()
    assert exact == [(7,)]
    assert con.sql(f"select count(*) from lance_vector_search('{indexed}', 'vector', {vector}, k=5)").fetchall() == [(5,)]
    apples = sum(1 for t in table["text"].to_pylist() if "apple" in t)
    assert con.sql(f"select count(*) from lance_fts('{indexed}', 'text', 'apple', k=10000)").fetchall() == [(apples,)]


def test_lancedb_reads_and_writes_with_nanolance(indexed, tmp_path):
    lancedb = pytest.importorskip("lancedb")
    import shutil

    shutil.copytree(indexed, tmp_path / "nano.lance")
    db = lancedb.connect(str(tmp_path))
    table = db.open_table("nano")
    ours = nl.dataset(str(tmp_path / "nano.lance"))
    assert table.count_rows() == 2000 and table.count_rows("id < 10") == 10
    assert table.to_arrow().sort_by("id").equals(ours.to_table())
    assert table.to_polars().collect().sort("id").equals(ours.to_polars().collect().sort("id"))
    assert sorted(i.index_type for i in table.list_indices()) == sorted(["BTree", "FTS", "IvfPq"])
    query = ours.take([7], columns=["vector"])["vector"][0].as_py()
    assert table.search(query).limit(3).to_polars()["id"][0] == 7
    apples = sum(1 for t in ours.to_table(columns=["text"])["text"].to_pylist() if "apple" in t)
    assert len(table.search("apple", query_type="fts").limit(10000).to_list()) == apples
    assert table.search().where("s = 'x3' AND id < 40").limit(100).to_pandas()["id"].tolist() == [3, 10, 17, 24, 31, 38]
    # LanceDB's own table, read and appended to by nanolance -- and nanolance's, appended to by LanceDB.
    theirs = db.create_table("theirs", _table(500))
    theirs.add(_table(10))
    theirs.delete("id = 5")  # in both batches: 508 rows
    path = str(tmp_path / "theirs.lance")
    assert nl.dataset(path).to_table().sort_by("id").equals(theirs.to_arrow().sort_by("id"))
    nl.write_dataset(_table(5), path, mode="append")
    table.add(_table(5))
    assert db.open_table("theirs").count_rows() == 513 and nl.dataset(str(tmp_path / "nano.lance")).count_rows() == 2005
