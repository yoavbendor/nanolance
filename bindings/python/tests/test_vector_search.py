"""Vector search: ``nearest=`` answered from Lance's IVF_FLAT / IVF_PQ indexes, or exactly.

The bar is pylance's own answer for the same dataset and query: the same rows in the same order with
the same ``_distance`` (to float tolerance) -- for an index pylance built, for the fragments it does
not cover, with filters before or after the search, deletions, and without an index. 4-bit PQ ranks
by quantized distances, so rows of equal distance may come in another order there.
"""

from __future__ import annotations

import importlib
import shutil

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance.dataset import VectorIndexReader as NanoVectorIndexReader
from tests.support import require_pylance

pytestmark = pytest.mark.filterwarnings("ignore::DeprecationWarning")

N, DIM = 3000, 16


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _vectors(n=N, seed=0):
    rng = np.random.default_rng(seed)
    return (rng.standard_normal((n, DIM)) * 2 + 0.5).astype(np.float32)


def _table(v, start=0, nulls=True):
    n = len(v)
    ids = np.arange(start, start + n)
    vec = pa.FixedSizeListArray.from_arrays(pa.array(v.ravel()), DIM)
    if nulls:
        mask = pa.array(ids % 97 == 5)
        vec = pa.FixedSizeListArray.from_arrays(pa.array(v.ravel()), DIM, mask=mask)
    return pa.table({"id": pa.array(ids), "g": pa.array(ids % 7), "vec": vec})


INDEXES = [
    ("IVF_FLAT", "l2", {}), ("IVF_FLAT", "cosine", {}), ("IVF_FLAT", "dot", {}),
    ("IVF_PQ", "l2", {"num_sub_vectors": 4}), ("IVF_PQ", "cosine", {"num_sub_vectors": 8}),
    ("IVF_PQ", "dot", {"num_sub_vectors": 4}), ("IVF_PQ", "l2", {"num_sub_vectors": 8, "num_bits": 4}),
]


@pytest.fixture(scope="module")
def indexed(lance, tmp_path_factory):
    """One dataset per index kind, indexed by pylance."""
    root = tmp_path_factory.mktemp("vs")
    out = {}
    for kind, metric, kw in INDEXES:
        path = str(root / f"{kind}_{metric}_{kw.get('num_bits', 8)}.lance")
        lance.write_dataset(_table(_vectors()), path, max_rows_per_file=1200)
        lance.dataset(path).create_index("vec", kind, metric=metric, num_partitions=8, **kw)
        out[kind, metric, kw.get("num_bits", 8)] = path
    return out


def _same(ref, got, four_bit=False, refined=False):
    rd, gd = ref.column("_distance").to_numpy(), got.column("_distance").to_numpy()
    assert len(rd) == len(gd)
    if not four_bit:
        assert np.allclose(rd, gd, rtol=1e-4, atol=1e-4), (rd[:5], gd[:5])
        assert ref.column("id").to_pylist() == got.column("id").to_pylist()
        return
    if not refined:
        # 4-bit PQ: which of equally (quantized) distant rows come first, or make the cut, is
        # arbitrary -- so the rows may differ there, but not the distances.
        assert np.allclose(rd, gd, rtol=1e-4, atol=1e-4), (rd[:5], gd[:5])
        return
    # Re-scored exactly (refine), different candidates at the cut can change the rows below it
    # too. The rows both return agree.
    ref_d = dict(zip(ref.column("id").to_pylist(), rd))
    got_d = dict(zip(got.column("id").to_pylist(), gd))
    shared = set(ref_d) & set(got_d)
    assert len(shared) >= 0.8 * len(ref_d), (sorted(ref_d), sorted(got_d))
    assert all(abs(ref_d[i] - got_d[i]) <= 1e-4 + 1e-4 * abs(ref_d[i]) for i in shared)


QUERIES = [
    {"k": 1}, {"k": 10}, {"k": 37}, {"k": 150}, {"k": 10, "nprobes": 2}, {"k": 10, "minimum_nprobes": 3},
    {"k": 10, "maximum_nprobes": 2}, {"k": 10, "refine_factor": 1}, {"k": 10, "refine_factor": 4},
    {"k": 20, "distance_range": (10.0, 60.0)},
]


@pytest.mark.parametrize("key", [(k, m, b) for k, m, kw in INDEXES for b in [kw.get("num_bits", 8)]],
                         ids=lambda key: "-".join(map(str, key)))
def test_matches_pylance_from_its_index(lance, indexed, key):
    path = indexed[key]
    v = _vectors()
    for qi in (0, 101, 2024):
        q = v[qi] + 0.25
        for extra in QUERIES:
            nearest = {"column": "vec", "q": q, **extra}
            ref = lance.dataset(path).to_table(nearest=nearest, columns=["id"])
            got = nl.dataset(path).to_table(nearest=nearest, columns=["id"])
            _same(ref, got, four_bit=key[2] == 4, refined="refine_factor" in extra)


@pytest.mark.parametrize("prefilter", [False, True])
@pytest.mark.parametrize("flt", ["g = 3", "id < 500", "g = 3 AND id > 2500", "id = 7"])
def test_filters_before_and_after_the_search(lance, indexed, flt, prefilter):
    for key in [("IVF_PQ", "l2", 8), ("IVF_FLAT", "cosine", 8)]:
        path = indexed[key]
        q = _vectors()[11]
        nearest = {"column": "vec", "q": q, "k": 10}
        ref = lance.dataset(path).to_table(nearest=nearest, filter=flt, prefilter=prefilter, columns=["id", "g"])
        got = nl.dataset(path).to_table(nearest=nearest, filter=flt, prefilter=prefilter, columns=["id", "g"])
        _same(ref, got)


def test_deleted_rows_and_unindexed_fragments(lance, indexed, tmp_path):
    for key in [("IVF_PQ", "l2", 8), ("IVF_FLAT", "l2", 8), ("IVF_PQ", "dot", 8)]:
        path = str(tmp_path / f"{key[0]}_{key[1]}.lance")
        shutil.copytree(indexed[key], path)
        v = _vectors()
        q = v[42] + 0.1
        # The nearest rows deleted: the search goes on to other rows (and partitions).
        lance.dataset(path).delete("id IN (42, 1, 2, 3) OR g = 2")
        for extra in ({"k": 10}, {"k": 10, "nprobes": 1}, {"k": 200, "nprobes": 1}):
            nearest = {"column": "vec", "q": q, **extra}
            _same(lance.dataset(path).to_table(nearest=nearest, columns=["id"]),
                  nl.dataset(path).to_table(nearest=nearest, columns=["id"]))
        # Rows the index does not cover are searched exactly (and the index's re-scored).
        extra_rows = _vectors(300, seed=9)
        extra_rows[0] = q
        nl.write_dataset(_table(extra_rows, start=N), path, mode="append")
        for extra in ({"k": 10}, {"k": 10, "refine_factor": 2}):
            nearest = {"column": "vec", "q": q, **extra}
            ref = lance.dataset(path).to_table(nearest=nearest, columns=["id"])
            got = nl.dataset(path).to_table(nearest=nearest, columns=["id"])
            _same(ref, got)
            if key[1] == "l2":
                assert got.column("id")[0].as_py() == N  # the appended copy of the query
            ref = lance.dataset(path).to_table(nearest=nearest, columns=["id"], fast_search=True)
            got = nl.dataset(path).to_table(nearest=nearest, columns=["id"], fast_search=True)
            _same(ref, got)
            assert N not in got.column("id").to_pylist()


def test_without_an_index(lance, tmp_path):
    path = str(tmp_path / "t.lance")
    nl.write_dataset(_table(_vectors()), path, max_rows_per_file=1000)
    q = _vectors()[3]
    for extra in ({"k": 5}, {"k": 50, "metric": "cosine"}, {"k": 5, "metric": "dot"}, {"k": N},
                  {"k": 20, "distance_range": (5.0, None)}):
        nearest = {"column": "vec", "q": q, **extra}
        ref = lance.dataset(path).to_table(nearest=nearest, columns=["id"])
        got = nl.dataset(path).to_table(nearest=nearest, columns=["id"])
        _same(ref, got)
    # Null vectors are never returned.
    got = nl.dataset(path).to_table(nearest={"column": "vec", "q": q, "k": N}, columns=["id"])
    assert got.num_rows == N - sum(1 for i in range(N) if i % 97 == 5)


def test_metric_other_than_the_index_and_use_index(lance, indexed):
    path = indexed["IVF_PQ", "l2", 8]
    q = _vectors()[5]
    for extra in ({"metric": "cosine"}, {"metric": "dot"}, {"use_index": False}, {"metric": "L2"}):
        nearest = {"column": "vec", "q": q, "k": 10, **extra}
        _same(lance.dataset(path).to_table(nearest=nearest, columns=["id"]),
              nl.dataset(path).to_table(nearest=nearest, columns=["id"]))


def test_output_shape(lance, indexed):
    path = indexed["IVF_FLAT", "l2", 8]
    q = _vectors()[8]
    nearest = {"column": "vec", "q": q.tolist(), "k": 6}
    for kw in ({}, {"columns": ["id"]}, {"with_row_id": True}, {"columns": ["g", "id"], "with_row_id": True},
               {"limit": 3}, {"columns": ["_distance", "id"]}):
        ref = lance.dataset(path).to_table(nearest=nearest, **kw)
        got = nl.dataset(path).to_table(nearest=nearest, **kw)
        assert ref.column_names == got.column_names, kw
        assert ref.schema.field("_distance").type == got.schema.field("_distance").type
        assert ref.num_rows == got.num_rows, kw
    ref = lance.dataset(path).scanner(nearest=nearest, columns=["id"]).to_table()
    got = nl.dataset(path).scanner(nearest=nearest, columns=["id"]).to_table()
    _same(ref, got)
    assert "ANNSubIndex" in nl.dataset(path).scanner(nearest=nearest).explain_plan()
    assert nl.dataset(path).list_indices()[0]["type"] == "IVF_FLAT"
    assert nl.dataset(indexed["IVF_PQ", "l2", 8]).describe_indices()[0].index_type == "IVF_PQ"


@pytest.mark.parametrize("columns", [
    ["id", "_rowid"], ["_rowid", "id"], ["id"], ["_distance", "_rowid", "id"],
    ["_rowid", "_rowaddr", "id"], ["id", "_rowaddr"],
])
def test_row_id_columns_go_last_as_pylance_orders_them(lance, indexed, tmp_path, columns):
    """`_distance` follows the named columns, then the row id columns, `_rowid` before `_rowaddr`, wherever
    they were named (pylance 12, flat and indexed, and fast_search)."""
    q = _vectors()[8].tolist()
    flat = str(tmp_path / "flat.lance")
    nl.write_dataset(_table(_vectors()), flat, max_rows_per_file=1000)
    for path in (flat, indexed["IVF_FLAT", "l2", 8]):
        for extra in ({}, {"fast_search": True}):
            nearest = {"column": "vec", "q": q, "k": 6}
            kw = {"columns": columns, "with_row_id": True, "with_row_address": "_rowaddr" in columns, **extra}
            ref = lance.dataset(path).to_table(nearest=nearest, **kw)
            got = nl.dataset(path).to_table(nearest=nearest, **kw)
            assert ref.column_names == got.column_names, (path, extra)
            assert got.column("_rowid").to_pylist() == ref.column("_rowid").to_pylist()


@pytest.mark.parametrize("columns", [None, ["id"], ["id", "_rowid"], ["_rowid", "id"], ["_distance", "id"]])
def test_fast_search_returns_rowid_as_pylance_does(lance, indexed, columns):
    """fast_search adds `_rowid` without with_row_id, before `_distance` (pylance 12)."""
    path = indexed["IVF_FLAT", "l2", 8]
    nearest = {"column": "vec", "q": _vectors()[8].tolist(), "k": 6}
    kw = {"fast_search": True}
    if columns is not None:
        kw["columns"] = columns
    ref = lance.dataset(path).to_table(nearest=nearest, **kw)
    got = nl.dataset(path).to_table(nearest=nearest, **kw)
    assert ref.column_names == got.column_names
    assert got.column("_rowid").to_pylist() == ref.column("_rowid").to_pylist()


@pytest.mark.parametrize("nearest,error", [
    ({"column": "nope", "q": [0.0] * DIM}, (ValueError, "Embedding column nope is not in the dataset")),
    ({"column": "id", "q": [0.0] * DIM}, (TypeError, "Query column id must be a vector")),
    ({"column": "vec", "q": [0.0] * (DIM + 1)}, (ValueError, f"Query vector size {DIM + 1} does not match")),
    ({"column": "vec", "q": None}, (TypeError, "Query vectors should be an array of floats")),
    ({"column": "vec", "q": [0.0] * DIM, "k": 0}, (ValueError, "Nearest-K must be > 0")),
    ({"column": "vec", "q": [0.0] * DIM, "nprobes": 0}, (ValueError, "Nprobes must be > 0")),
    ({"column": "vec", "q": [0.0] * DIM, "refine_factor": 0}, (ValueError, "Refine factor must be 1 or more")),
])
def test_errors_as_pylance_raises_them(lance, indexed, nearest, error):
    path = indexed["IVF_FLAT", "l2", 8]
    kind, message = error
    with pytest.raises(kind, match=message):
        lance.dataset(path).to_table(nearest=nearest)
    with pytest.raises(kind, match=message):
        nl.dataset(path).to_table(nearest=nearest)


# ── indexes nanolance builds ────────────────────────────────────────────────────────────────────

def _clustered(n=6000, seed=3):
    rng = np.random.default_rng(seed)
    centers = rng.standard_normal((40, DIM)) * 3
    return (centers[rng.integers(0, 40, n)] + rng.standard_normal((n, DIM))).astype(np.float32)


def _exact_ids(v, q, metric, k):
    if metric == "l2":
        d = ((v - q) ** 2).sum(1)
    elif metric == "cosine":
        d = 1 - v @ q / (np.linalg.norm(v, axis=1) * np.linalg.norm(q))
    else:
        d = 1 - v @ q
    return set(np.argsort(d, kind="stable")[:k].tolist())


@pytest.mark.parametrize("kind,metric,kw,min_recall", [
    ("IVF_FLAT", "l2", {}, 0.99), ("IVF_FLAT", "cosine", {}, 0.99), ("IVF_FLAT", "dot", {}, 0.99),
    ("IVF_PQ", "l2", {"num_sub_vectors": 8}, 0.3), ("IVF_PQ", "cosine", {"num_sub_vectors": 8}, 0.3),
    ("IVF_PQ", "dot", {"num_sub_vectors": 8}, 0.2), ("IVF_PQ", "l2", {"num_sub_vectors": 8, "num_bits": 4}, 0.08),
])
def test_pylance_searches_nanolances_index(lance, tmp_path, kind, metric, kw, min_recall):
    v = _clustered()
    path = str(tmp_path / "t.lance")
    nl.write_dataset(_table(v, nulls=False), path, max_rows_per_file=2500)
    nl.dataset(path).create_index("vec", kind, metric=metric, num_partitions=12, **kw)
    ds = lance.dataset(path)
    assert ds.list_indices()[0]["type"] == kind
    stats = ds.stats.index_stats("vec_idx")
    assert stats["index_type"] == kind and stats["num_indexed_rows"] == len(v) and stats["num_unindexed_rows"] == 0
    rng = np.random.default_rng(1)
    recall = []
    for qi in rng.integers(0, len(v), 15):
        q = v[qi] + rng.standard_normal(DIM).astype(np.float32) * 0.2
        for extra in ({"k": 10}, {"k": 10, "nprobes": 12}, {"k": 25, "refine_factor": 2}):
            nearest = {"column": "vec", "q": q, **extra}
            ref = ds.to_table(nearest=nearest, columns=["id"])
            got = nl.dataset(path).to_table(nearest=nearest, columns=["id"])
            _same(ref, got, four_bit=kw.get("num_bits") == 4, refined="refine_factor" in extra)
        all_partitions = ds.to_table(nearest={"column": "vec", "q": q, "k": 10, "nprobes": 12}, columns=["id"])
        recall.append(len(set(all_partitions.column("id").to_pylist()) & _exact_ids(v, q, metric, 10)) / 10)
    assert np.mean(recall) >= min_recall, np.mean(recall)


def test_build_defaults_and_maintenance(lance, tmp_path):
    v = _clustered(9000)
    path = str(tmp_path / "t.lance")
    nl.write_dataset(_table(v, nulls=True), path)
    nl.dataset(path).create_index("vec", "IVF_FLAT")  # 9000 / 4096 rows a partition: 2 partitions
    ds = lance.dataset(path)
    assert ds.stats.index_stats("vec_idx")["indices"][0]["num_partitions"] == 2
    nulls = sum(1 for i in range(len(v)) if i % 97 == 5)
    assert ds.stats.index_stats("vec_idx")["num_indexed_rows"] == len(v)  # pylance counts the fragments' rows
    got = nl.dataset(path).to_table(nearest={"column": "vec", "q": v[1], "k": len(v)}, columns=["id"])
    assert got.num_rows == len(v) - nulls
    # Replaced, named, then appended to and optimized by pylance.
    with pytest.raises(RuntimeError, match="already exists"):
        nl.dataset(path).create_index("vec", "IVF_PQ", num_partitions=4, num_sub_vectors=4)
    nl.dataset(path).create_index("vec", "IVF_PQ", num_partitions=4, num_sub_vectors=4, replace=True)
    assert [i["type"] for i in lance.dataset(path).list_indices()] == ["IVF_PQ"]
    nl.dataset(path).create_index("vec", "IVF_FLAT", name="flat_idx", num_partitions=3)
    assert sorted(i["name"] for i in lance.dataset(path).list_indices()) == ["flat_idx", "vec_idx"]
    nl.dataset(path).drop_index("flat_idx")
    extra = _clustered(400, seed=8)
    nl.write_dataset(_table(extra, start=len(v), nulls=False), path, mode="append")
    q = extra[7]
    for ds in (lance.dataset(path),):
        ds.optimize.optimize_indices()
    ds = lance.dataset(path)
    assert ds.stats.index_stats("vec_idx")["num_unindexed_rows"] == 0
    nearest = {"column": "vec", "q": q, "k": 10}
    _same(ds.to_table(nearest=nearest, columns=["id"]), nl.dataset(path).to_table(nearest=nearest, columns=["id"]))
    # PQ distances are approximate: re-scored exactly, the appended copy of the query comes first.
    exact = {**nearest, "refine_factor": 5}
    assert nl.dataset(path).to_table(nearest=exact, columns=["id"]).column("id")[0].as_py() == len(v) + 7


@pytest.mark.parametrize("n,kw,error", [
    (1000, {"index_type": "IVF_PQ", "num_partitions": 4}, (ValueError, "num_sub_vectors are required")),
    (1000, {"index_type": "IVF_PQ", "num_partitions": 4, "num_sub_vectors": 5},
     (ValueError, r"dimension \(16\) must be divisible by num_sub_vectors \(5\)")),
    (100, {"index_type": "IVF_FLAT", "num_partitions": 200}, (RuntimeError, "KMeans cannot train 200 centroids")),
    (100, {"index_type": "IVF_PQ", "num_partitions": 2, "num_sub_vectors": 4},
     (RuntimeError, "Not enough rows to train PQ")),
    (1000, {"index_type": "IVF_PQ", "num_partitions": 4, "num_sub_vectors": 4, "num_bits": 3},
     (RuntimeError, "num_bits 3 not supported")),
    (1000, {"index_type": "IVF_XYZ", "num_partitions": 4}, (NotImplementedError, "index types supported")),
])
def test_build_errors_as_pylance_raises_them(lance, tmp_path, n, kw, error):
    kind, message = error
    for mod in (lance, nl):
        path = str(tmp_path / f"{mod.__name__}.lance")
        shutil.rmtree(path, ignore_errors=True)
        lance.write_dataset(_table(_vectors(n), nulls=False), path)
        with pytest.raises(kind, match=message):
            mod.dataset(path).create_index("vec", **kw)
    with pytest.raises(TypeError, match="Vector column id must be"):
        nl.dataset(path).create_index("id", "IVF_FLAT", num_partitions=2)


READER_INDEXES = [
    ("IVF_FLAT", {}), ("IVF_PQ", {"num_sub_vectors": 4}), ("IVF_PQ", {"num_sub_vectors": 4, "num_bits": 4}),
    ("IVF_SQ", {}), ("IVF_HNSW_SQ", {}), ("IVF_HNSW_PQ", {"num_sub_vectors": 4}), ("IVF_HNSW_FLAT", {}),
]


def _partitions(reader, with_vector):
    return [reader.read_partition(p, with_vector=with_vector) for p in range(reader.num_partitions())]


@pytest.mark.parametrize("builder", ["pylance", "nanolance"])
@pytest.mark.parametrize("kind,kw", READER_INDEXES, ids=lambda x: str(x))
def test_vector_index_reader_matches_pylance(lance, tmp_path, builder, kind, kw):
    """VectorIndexReader returns each partition as pylance does: the same schema, the same rows in the
    same order, the same values -- for an index either engine built."""
    path = str(tmp_path / "t.lance")
    table = _table(_vectors(900), nulls=False)
    if builder == "pylance":
        lance.write_dataset(table, path, max_rows_per_file=400)
        lance.dataset(path).create_index("vec", kind, num_partitions=6, **kw)
    else:
        nl.write_dataset(table, path, max_rows_per_file=400)
        nl.dataset(path).create_index("vec", kind, num_partitions=6, **kw)
    ref_ds, got_ds = lance.dataset(path), nl.dataset(path)
    ref_reader = importlib.import_module(f"{lance.__name__}.dataset").VectorIndexReader
    ref = ref_reader(ref_ds, ref_ds.describe_indices()[0].name)
    got = NanoVectorIndexReader(got_ds, got_ds.describe_indices()[0].name)
    assert got.num_partitions() == ref.num_partitions() == 6
    for with_vector in (False, True):
        for a, b in zip(_partitions(ref, with_vector), _partitions(got, with_vector)):
            assert b.schema.equals(a.schema, check_metadata=False), (b.schema, a.schema)
            assert b.equals(a)
    assert sum(p.num_rows for p in _partitions(got, True)) == 900
    column = "flat" if kind.endswith("FLAT") else ("__pq_code" if "PQ" in kind else "__sq_code")
    assert all(column in p.column_names for p in _partitions(got, True))


def test_vector_index_reader_errors_as_pylance_raises_them(lance, tmp_path):
    path = str(tmp_path / "t.lance")
    lance.write_dataset(_table(_vectors(300), nulls=False), path)
    lance.dataset(path).create_index("vec", "IVF_FLAT", num_partitions=4)
    lance.dataset(path).create_scalar_index("id", index_type="BTREE")
    ds = nl.dataset(path)
    reader = NanoVectorIndexReader(ds, "vec_idx")
    with pytest.raises(IndexError, match="out of range"):
        reader.read_partition(reader.num_partitions())
    with pytest.raises(IndexError, match="out of range"):
        reader.read_partition(-1)
    with pytest.raises(ValueError, match="not vector index"):
        NanoVectorIndexReader(ds, "id_idx")
    with pytest.raises(KeyError):
        NanoVectorIndexReader(ds, "missing_idx")


def test_nearest_on_nested_vector_covers_unindexed_fragments(lance, tmp_path):
    """A KNN on `data.embedding` after an append searches the unindexed fragment too, as pylance does."""
    dim, n = 32, 256
    v = np.random.default_rng(3).standard_normal((n, dim)).astype(np.float32)
    struct = pa.struct([pa.field("embedding", pa.list_(pa.float32(), dim)), pa.field("label", pa.string())])
    schema = pa.schema([pa.field("id", pa.int64()), pa.field("data", struct)])

    def table(rows, ids):
        vec = pa.FixedSizeListArray.from_arrays(pa.array(rows.flatten()), dim)
        data = pa.StructArray.from_arrays([vec, pa.array([f"l{i}" for i in ids])], names=["embedding", "label"])
        return pa.table({"id": list(ids), "data": data}, schema=schema)

    got = {}
    for name, engine in (("nano", nl), ("pylance", lance)):
        uri = str(tmp_path / name)
        engine.write_dataset(table(v, range(n)), uri)
        engine.dataset(uri).create_index(column="data.embedding", index_type="IVF_FLAT", num_partitions=2)
        ds = engine.write_dataset(table(v[:10], range(n, n + 10)), uri, mode="append")
        got[name] = ds.to_table(columns=["id"], nearest={"column": "data.embedding", "q": v[0], "k": 15,
                                                         "nprobes": 2})["id"].to_pylist()
    assert got["nano"] == got["pylance"]
    assert {0, n} <= set(got["nano"])
