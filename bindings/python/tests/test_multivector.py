"""Multivector search (ColBERT-style: a list of fixed-size lists): flat search, the bar being pylance's answer.

A row's distance sums, over the query's vectors, the least distance to any of the row's vectors. Rows
with a null or empty vector list are left out, as Lance leaves them out. Over an IVF index (any kind pylance
builds over a multivector column, cosine only) the search is Lance's XTR (the candidates per query vector, then
the sum; see the index tests at the end), and refine_factor rescores the candidates exactly as Lance does.
"""

from __future__ import annotations

import collections
import glob
import uuid

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

pytestmark = pytest.mark.filterwarnings("ignore::DeprecationWarning")

DIM = 4
ROWS = 60


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _rows(rng, n=ROWS):
    """Each row: several vectors, some null, some empty."""
    rows = []
    for i in range(n):
        if i % 11 == 3:
            rows.append(None)
        elif i % 13 == 5:
            rows.append([])
        else:
            rows.append(rng.standard_normal((int(rng.integers(1, 5)), DIM)).tolist())
    return rows


def _dataset(lance, tmp_path, elem, seed=1):
    """A dataset whose ``vec`` is list<list<elem, DIM>>, written by pylance."""
    rng = np.random.default_rng(seed)
    rows = _rows(rng)
    if elem == "uint8":
        typ = pa.uint8()
        rows = [None if r is None else [[int(x) % 256 for x in v] for v in r] for r in rows]
    else:
        typ = {"float32": pa.float32(), "float16": pa.float16(), "float64": pa.float64()}[elem]
    table = pa.table({"id": pa.array(np.arange(len(rows)), pa.int64()), "g": pa.array(np.arange(len(rows)) % 3),
                      "vec": pa.array(rows, type=pa.list_(pa.list_(typ, DIM)))})
    path = str(tmp_path / f"{elem}_{uuid.uuid4().hex[:8]}.lance")
    lance.write_dataset(table, path)
    return path


def _query(elem, seed=2, vectors=2):
    rng = np.random.default_rng(seed)
    if elem == "uint8":
        shape = (vectors, DIM) if vectors > 1 else (DIM,)
        return rng.integers(0, 256, shape).astype(np.uint8).tolist()
    return rng.standard_normal((vectors, DIM)).astype(np.float32).tolist()


def _same(ref, got, rtol=1e-6, atol=1e-6):
    """The same rows in the same order, with the same distances (NaN rows are absent from both)."""
    assert ref["id"].to_pylist() == got["id"].to_pylist()
    rd, gd = ref["_distance"].to_numpy(), got["_distance"].to_numpy()
    assert not np.isnan(rd).any() and not np.isnan(gd).any()
    np.testing.assert_allclose(gd, rd, rtol=rtol, atol=atol)


@pytest.mark.parametrize("elem, metric", [
    ("float32", "l2"), ("float32", "cosine"), ("float32", "dot"),
    ("float16", "l2"), ("float16", "cosine"), ("float16", "dot"),
    ("float64", "l2"), ("float64", "cosine"), ("float64", "dot"),
    ("uint8", "hamming"),
])
def test_flat_distances_match_pylance(lance, tmp_path, elem, metric):
    path = _dataset(lance, tmp_path, elem)
    nearest = {"column": "vec", "q": _query(elem, vectors=1 if elem == "uint8" else 2), "k": 60, "metric": metric,
               "use_index": False}
    ref = lance.dataset(path).to_table(columns=["id"], nearest=nearest)
    got = nl.dataset(path).to_table(columns=["id"], nearest=nearest)
    assert 0 < len(ref) < ROWS  # null and empty rows are left out by both
    _same(ref, got)


@pytest.mark.parametrize("metric", ["l2", "cosine", "dot"])
def test_float16_query_is_rounded_to_the_column_type(lance, tmp_path, metric):
    path = _dataset(lance, tmp_path, "float16")
    nearest = {"column": "vec", "q": _query("float16", vectors=3), "k": 60, "metric": metric, "use_index": False}
    _same(lance.dataset(path).to_table(columns=["id"], nearest=nearest),
          nl.dataset(path).to_table(columns=["id"], nearest=nearest))


def test_no_metric_is_l2(lance, tmp_path):
    path = _dataset(lance, tmp_path, "float32")
    nearest = {"column": "vec", "q": _query("float32"), "k": 60, "use_index": False}
    _same(lance.dataset(path).to_table(columns=["id"], nearest=nearest),
          nl.dataset(path).to_table(columns=["id"], nearest=nearest))


def test_single_vector_query_on_a_multivector_column(lance, tmp_path):
    path = _dataset(lance, tmp_path, "float32")
    nearest = {"column": "vec", "q": [0.5, -1.0, 0.25, 2.0], "k": 60, "metric": "cosine", "use_index": False}
    _same(lance.dataset(path).to_table(columns=["id"], nearest=nearest),
          nl.dataset(path).to_table(columns=["id"], nearest=nearest))


@pytest.mark.parametrize("prefilter", [False, True])
def test_filter_before_and_after_the_search(lance, tmp_path, prefilter):
    path = _dataset(lance, tmp_path, "float32")
    nearest = {"column": "vec", "q": _query("float32"), "k": 60, "metric": "cosine", "use_index": False}
    kw = {"filter": "g = 1", "prefilter": prefilter}
    ref = lance.dataset(path).to_table(columns=["id"], nearest=nearest, **kw)
    got = nl.dataset(path).to_table(columns=["id"], nearest=nearest, **kw)
    assert len(ref) > 0
    _same(ref, got)


def test_result_column_types(lance, tmp_path):
    path = _dataset(lance, tmp_path, "float32")
    nearest = {"column": "vec", "q": _query("float32"), "k": 5, "use_index": False}
    got = nl.dataset(path).to_table(nearest=nearest)
    assert got.schema.field("vec").type == pa.list_(pa.list_(pa.float32(), DIM))
    assert got.schema.field("_distance").type == pa.float32()
    assert got.column("vec")[0].as_py() is not None


def test_mixed_indexed_and_appended_rows_match_pylance(lance, tmp_path):
    """A pylance-built IVF_FLAT index that covers the first fragment only: the appended rows are scored exactly."""
    vt = pa.list_(pa.list_(pa.float32(), 2))
    path = str(tmp_path / "indexed.lance")
    lance.write_dataset(pa.table({"id": pa.array([0, 1], pa.int32()),
                                  "vector": pa.array([[[1.0, 0.0], [0.0, 1.0]], [[1.0, 0.0], [1.0, 0.0]]],
                                                     type=vt)}), path)
    lance.dataset(path).create_index("vector", index_type="IVF_FLAT", metric="cosine", num_partitions=1)
    lance.write_dataset(pa.table({"id": pa.array([2, 3], pa.int32()),
                                  "vector": pa.array([[[1.0, 0.0], [0.0, 1.0]], [[-1.0, 0.0], [0.0, -1.0]]],
                                                     type=vt)}), path, mode="append")
    nearest = {"column": "vector", "q": np.array([[1.0, 0.0], [0.0, 1.0]], np.float32), "k": 4, "metric": "cosine"}
    for kw in ({"use_index": False}, {}):
        ref = lance.dataset(path).to_table(columns=["id"], nearest={**nearest, **kw})
        got = nl.dataset(path).to_table(columns=["id"], nearest={**nearest, **kw})
        assert got["id"].to_pylist() == ref["id"].to_pylist() == [0, 2, 1, 3]
        np.testing.assert_allclose(got["_distance"].to_numpy(), [0.0, 0.0, 1.0, 2.0])


@pytest.mark.parametrize("query, message", [
    ([[1.0, 0.0, 0.0], [0.0, 1.0, 0.0]], f"Query vector size 2 does not match index column size {DIM}"),
    ([1.0, 0.0, 0.0], f"Query vector size 3 does not match index column size {DIM}"),
    ([[1.0, 0.0], [0.0, 1.0, 0.0]], "All query vectors must have the same length, but got 2 and 3"),
])
def test_query_shape_is_checked_as_pylance_checks_it(lance, tmp_path, query, message):
    path = _dataset(lance, tmp_path, "float32")
    nearest = {"column": "vec", "q": query, "k": 3, "use_index": False}
    with pytest.raises(ValueError, match=message) as got:
        nl.dataset(path).to_table(columns=["id"], nearest=nearest)
    with pytest.raises(ValueError) as ref:
        lance.dataset(path).to_table(columns=["id"], nearest=nearest)
    assert str(got.value) == str(ref.value)


def test_metric_must_fit_the_element_type(lance, tmp_path):
    path = _dataset(lance, tmp_path, "float32")
    with pytest.raises(ValueError, match="does not support Float32 vectors"):
        nl.dataset(path).to_table(columns=["id"], nearest={"column": "vec", "q": _query("float32"), "k": 3,
                                                            "metric": "hamming", "use_index": False})
    upath = _dataset(lance, tmp_path, "uint8")
    for metric in ("l2", "cosine"):
        with pytest.raises(ValueError, match=f"Distance type {metric} does not support UInt8 vectors"):
            nl.dataset(upath).to_table(columns=["id"], nearest={"column": "vec", "q": _query("uint8", vectors=1), "k": 3,
                                                                 "metric": metric, "use_index": False})


def test_multivector_uint8_query_is_refused_as_pylance_refuses_it(lance, tmp_path):
    upath = _dataset(lance, tmp_path, "uint8")
    nearest = {"column": "vec", "q": _query("uint8", vectors=2), "k": 3, "metric": "hamming", "use_index": False}
    with pytest.raises(ValueError, match="UInt8 and the query vector is Float32"):
        lance.dataset(upath).to_table(columns=["id"], nearest=nearest)
    with pytest.raises(ValueError, match="UInt8 and the query vector is Float32"):
        nl.dataset(upath).to_table(columns=["id"], nearest=nearest)


# -- IVF indexes over a multivector column: Lance's XTR and refine (pylance builds these with cosine only) --


def _dense(rng, n):
    """Rows of 1 to 4 vectors each, none null or empty: every row has vectors in the index."""
    return [rng.standard_normal((int(rng.integers(1, 5)), DIM)).astype(np.float32).tolist() for _ in range(n)]


# The index kinds pylance 12 builds over a multivector column (cosine only; L2 and dot builds are refused).
INDEX_KINDS = [("IVF_FLAT", {}), ("IVF_SQ", {}), ("IVF_PQ", {"num_sub_vectors": 2}), ("IVF_HNSW_FLAT", {}),
               ("IVF_HNSW_SQ", {}), ("IVF_HNSW_PQ", {"num_sub_vectors": 2})]


def _ivf_flat_dataset(lance, tmp_path, name, rows, indexed, parts, kind="IVF_FLAT", kw=None, elem=pa.float32()):
    """``rows`` written by pylance: the first ``indexed`` under a cosine index of ``kind``, the rest appended."""
    path = str(tmp_path / f"{name}.lance")
    vt = pa.list_(pa.list_(elem, DIM))

    def table(lo, hi):
        return pa.table({"id": pa.array(np.arange(lo, hi), pa.int64()), "vec": pa.array(rows[lo:hi], type=vt)})

    lance.write_dataset(table(0, indexed), path)
    lance.dataset(path).create_index("vec", index_type=kind, metric="cosine", num_partitions=parts, **(kw or {}))
    if indexed < len(rows):
        lance.write_dataset(table(indexed, len(rows)), path, mode="append")
    return path


@pytest.mark.parametrize("parts", [2, 8])
@pytest.mark.parametrize("mixed", [False, True])
def test_ivf_flat_xtr_matches_pylance(lance, tmp_path, parts, mixed):
    """k * 10 covers every indexed vector (every row in every list) and small nprobes miss rows; with and
    without appended rows; cosine and L2 queries (L2 falls back to exact search): pylance's rows, order and
    distances to 1e-6."""
    rng = np.random.default_rng(100 * parts + mixed)
    rows = _dense(rng, 120)
    path = _ivf_flat_dataset(lance, tmp_path, f"xtr_{parts}_{mixed}", rows, 80 if mixed else 120, parts)
    query = rng.standard_normal((3, DIM)).astype(np.float32)
    for metric in ("cosine", "l2"):
        for nprobes in (1, 2, parts):
            for k in (5, 50):
                nearest = {"column": "vec", "q": query, "k": k, "metric": metric, "nprobes": nprobes}
                ref = lance.dataset(path).to_table(columns=["id"], nearest=nearest)
                got = nl.dataset(path).to_table(columns=["id"], nearest=nearest)
                _same(ref, got)


@pytest.mark.parametrize("parts, nprobes", [(2, 1), (8, 8)])
def test_ivf_flat_fast_search_matches_pylance(lance, tmp_path, parts, nprobes):
    """fast_search leaves the appended rows out and answers the indexed rows by XTR, as pylance does."""
    rng = np.random.default_rng(7 + parts)
    rows = _dense(rng, 120)
    path = _ivf_flat_dataset(lance, tmp_path, f"fast_{parts}", rows, 80, parts)
    nearest = {"column": "vec", "q": rng.standard_normal((3, DIM)).astype(np.float32), "k": 50,
               "metric": "cosine", "nprobes": nprobes}
    ref = lance.dataset(path).to_table(columns=["id"], nearest=nearest, fast_search=True)
    got = nl.dataset(path).to_table(columns=["id"], nearest=nearest, fast_search=True)
    assert max(ref["id"].to_pylist()) < 80
    _same(ref, got)


def test_ivf_flat_after_optimize_indices_matches_pylance(lance, tmp_path):
    """Once optimize_indices has indexed the appended rows, both the plain and the fast search match pylance."""
    rng = np.random.default_rng(11)
    rows = _dense(rng, 120)
    path = _ivf_flat_dataset(lance, tmp_path, "optimized", rows, 80, 4)
    lance.dataset(path).optimize.optimize_indices()
    nearest = {"column": "vec", "q": rng.standard_normal((3, DIM)).astype(np.float32), "k": 50,
               "metric": "cosine", "nprobes": 1}
    for kw in ({}, {"fast_search": True}):
        _same(lance.dataset(path).to_table(columns=["id"], nearest=nearest, **kw),
              nl.dataset(path).to_table(columns=["id"], nearest=nearest, **kw))


@pytest.mark.parametrize("kind, kw", INDEX_KINDS)
@pytest.mark.parametrize("mixed", [False, True])
def test_index_kinds_xtr_match_pylance(lance, tmp_path, kind, kw, mixed):
    """Every kind pylance builds over a multivector column is searched by XTR: pylance's rows, order and
    distances to 1e-6, with and without appended (unindexed) rows, plain and fast_search."""
    rng = np.random.default_rng(31 + 2 * mixed)
    rows = _dense(rng, 240)  # enough vectors to train PQ (256) in the indexed part
    path = _ivf_flat_dataset(lance, tmp_path, f"xtr_{kind}_{mixed}", rows, 160 if mixed else 240, 2, kind, kw)
    query = rng.standard_normal((3, DIM)).astype(np.float32)
    for nprobes in (1, 2):
        for k in (5, 50):
            for extra in ({}, {"fast_search": True}) if not mixed else ({},):
                nearest = {"column": "vec", "q": query, "k": k, "metric": "cosine", "nprobes": nprobes}
                ref = lance.dataset(path).to_table(columns=["id"], nearest=nearest, **extra)
                got = nl.dataset(path).to_table(columns=["id"], nearest=nearest, **extra)
                _same(ref, got)


@pytest.mark.parametrize("kind, kw", INDEX_KINDS)
@pytest.mark.parametrize("refine", [1, 2, 10])
def test_refine_factor_matches_pylance(lance, tmp_path, kind, kw, refine):
    """refine_factor on a multivector index (Lance's scanner): the XTR candidates, k * refine of them, are
    re-scored exactly and the top k kept; with appended rows the same before the unindexed rows merge in."""
    rng = np.random.default_rng(41 + refine)
    rows = _dense(rng, 480)  # enough vectors to train PQ (256) in the indexed part
    path = _ivf_flat_dataset(lance, tmp_path, f"refine_{kind}_{refine}", rows, 400, 2, kind, kw)
    query = rng.standard_normal((3, DIM)).astype(np.float32)
    for nprobes in (1, 2):
        nearest = {"column": "vec", "q": query, "k": 5, "metric": "cosine", "nprobes": nprobes,
                   "refine_factor": refine}
        ref = lance.dataset(path).to_table(columns=["id"], nearest=nearest)
        got = nl.dataset(path).to_table(columns=["id"], nearest=nearest)
        _same(ref, got)


@pytest.mark.parametrize("kind, kw", INDEX_KINDS)
def test_float16_index_matches_pylance(lance, tmp_path, kind, kw):
    """float16 multivector indexes. With refine_factor the answer is exact: pylance's rows and distances to
    1e-6. Without it the XTR distances of a float16 index differ from pylance's by up to about 2e-4 (the same
    gap as a plain float16 IVF_FLAT search here: the cause is not explained yet), so there the rows and their
    order must match and the distances agree to 5e-4."""
    rng = np.random.default_rng(53)
    rows = _dense(rng, 480)  # enough vectors to train PQ (256) in the indexed part
    path = _ivf_flat_dataset(lance, tmp_path, f"f16_{kind}", rows, 400, 2, kind, kw, elem=pa.float16())
    query = rng.standard_normal((3, DIM)).astype(np.float32)
    for nprobes in (1, 2):
        for refine in (None, 10):
            nearest = {"column": "vec", "q": query, "k": 5, "metric": "cosine", "nprobes": nprobes}
            if refine:
                nearest["refine_factor"] = refine
            ref = lance.dataset(path).to_table(columns=["id"], nearest=nearest)
            got = nl.dataset(path).to_table(columns=["id"], nearest=nearest)
            assert got["id"].to_pylist() == ref["id"].to_pylist()
            _same(ref, got, rtol=1e-6 if refine else 5e-4, atol=1e-6 if refine else 5e-4)



# -- Builds: nanolance writes the indexes pylance 12 writes over a multivector column (M3) --


def _table(rows, elem, dim=DIM):
    vt = pa.list_(pa.list_(elem, dim))
    return pa.table({"id": pa.array(np.arange(len(rows)), pa.int64()), "vec": pa.array(rows, type=vt)})


def _built_dataset(tmp_path, name, rows, kind, kw=None, elem=pa.float32(), parts=2):
    """``rows`` written and indexed by nanolance: a cosine index of ``kind`` over the whole column."""
    path = str(tmp_path / f"{name}.lance")
    nl.write_dataset(_table(rows, elem), path)
    nl.dataset(path).create_index("vec", index_type=kind, metric="cosine", num_partitions=parts, **(kw or {}))
    return path


def _same_modulo_ties(ref, got, tol):
    """float16 distances differ by about 1e-3 between the engines, so rows whose distances tie (or nearly)
    may swap places: the distances agree rank by rank to ``tol``, and every row clearly inside the top k
    (more than 2 * tol from the k-th distance) is in both answers."""
    rd, gd = ref["_distance"].to_numpy(), got["_distance"].to_numpy()
    assert len(rd) == len(gd)
    np.testing.assert_allclose(gd, rd, rtol=0, atol=tol)
    rids, gids = ref["id"].to_pylist(), got["id"].to_pylist()
    cut = rd[-1] - 2 * tol if len(rd) else 0.0
    for rid, d in zip(rids, rd):
        assert d >= cut or rid in gids, (rid, d)
    for gid, d in zip(gids, gd):
        assert d >= cut or gid in rids, (gid, d)


@pytest.mark.parametrize("kind, kw", INDEX_KINDS)
@pytest.mark.parametrize("elem", [pa.float32(), pa.float16()])
def test_nanolance_built_index_is_searched_as_pylance_searches_it(lance, tmp_path, kind, kw, elem):
    """Every kind nanolance builds over a multivector column: pylance searches it, and nanolance searches
    it. Float32: the same rows and order, plain and fast_search, distances to 1e-6. Float16: the distances
    agree to 2e-3 and the rows agree up to near-ties (see _same_modulo_ties)."""
    rng = np.random.default_rng(61)
    rows = _dense(rng, 480)  # enough vectors to train PQ (256) on
    path = _built_dataset(tmp_path, f"built_{kind}_{elem}", rows, kind, kw, elem)
    query = rng.standard_normal((3, DIM)).astype(np.float32)
    f32 = elem == pa.float32()
    for nprobes in (1, 2):
        for k in (5, 50):
            for extra in ({}, {"fast_search": True}) if f32 else ({},):
                nearest = {"column": "vec", "q": query, "k": k, "metric": "cosine", "nprobes": nprobes}
                ref = nl.dataset(path).to_table(columns=["id"], nearest=nearest, **extra)
                got = lance.dataset(path).to_table(columns=["id"], nearest=nearest, **extra)
                if f32:
                    _same(ref, got)
                else:
                    _same_modulo_ties(ref, got, 2e-3)


@pytest.mark.parametrize("elem", [pa.float32(), pa.float16()])
def test_built_index_layout_matches_pylance(lance, tmp_path, elem):
    """The files of an IVF_FLAT build: auxiliary.idx holds one row per vector (a row id per vector, the
    vectors as halffloat for float16), with pylance's schema and metadata; index.idx has pylance's metadata."""
    from lance.file import LanceFileReader

    rng = np.random.default_rng(71)
    rows = _dense(rng, 120)
    built = _built_dataset(tmp_path, f"layout_n_{elem}", rows, "IVF_FLAT", None, elem)
    made = _ivf_flat_dataset(lance, tmp_path, f"layout_p_{elem}", rows, 120, 2, "IVF_FLAT", None, elem)

    def files(path, name):
        return glob.glob(f"{path}/_indices/*/{name}")[0]

    aux_n = LanceFileReader(files(built, "auxiliary.idx")).read_all().to_table()
    aux_p = LanceFileReader(files(made, "auxiliary.idx")).read_all().to_table()
    assert aux_n.schema.types == aux_p.schema.types
    assert aux_n.schema.metadata == aux_p.schema.metadata
    total = sum(len(r) for r in rows)
    assert aux_n.num_rows == total
    counts = collections.Counter(aux_n.column("_rowid").to_pylist())
    assert counts == {i: len(r) for i, r in enumerate(rows)}  # each row id once per vector
    idx_n = LanceFileReader(files(built, "index.idx")).metadata().schema.metadata
    idx_p = LanceFileReader(files(made, "index.idx")).metadata().schema.metadata
    assert idx_n == idx_p


@pytest.mark.parametrize("metric", ["l2", "dot"])
def test_build_refuses_non_cosine_as_pylance_does(lance, tmp_path, metric):
    rng = np.random.default_rng(81)
    rows = _dense(rng, 60)
    message = "multivector type supports only cosine distance"
    path = str(tmp_path / f"refuse_{metric}.lance")
    lance.write_dataset(_table(rows, pa.float32()), path)
    with pytest.raises(RuntimeError, match=message):
        lance.dataset(path).create_index("vec", index_type="IVF_FLAT", metric=metric, num_partitions=2)
    with pytest.raises(RuntimeError, match=message):
        nl.dataset(path).create_index("vec", index_type="IVF_FLAT", metric=metric, num_partitions=2)
    assert nl.dataset(path).list_indices() == []


def test_optimize_indices_after_nanolance_build_matches_pylance(lance, tmp_path):
    """Rows appended after a nanolance build are unindexed until optimize_indices indexes them; then both
    engines' plain and fast searches agree on the whole column."""
    rng = np.random.default_rng(91)
    rows = _dense(rng, 200)
    path = _built_dataset(tmp_path, "optimize_built", rows[:120], "IVF_FLAT", None, pa.float32(), parts=4)
    nl.write_dataset(_table(rows, pa.float32()).slice(120), path, mode="append")
    nl.dataset(path).optimize.optimize_indices()
    query = rng.standard_normal((3, DIM)).astype(np.float32)
    for extra in ({}, {"fast_search": True}):
        nearest = {"column": "vec", "q": query, "k": 50, "metric": "cosine", "nprobes": 2}
        _same(lance.dataset(path).to_table(columns=["id"], nearest=nearest, **extra),
              nl.dataset(path).to_table(columns=["id"], nearest=nearest, **extra))


def _topic_rows(rng, n, vectors, dim):
    """One topic per row: each row's vectors are its topic plus noise, so a query made of a row's vectors
    has a few true neighbours (the recall is meaningful, unlike on random rows)."""
    centers = rng.standard_normal((n, dim)).astype(np.float32)
    rows = [(centers[r] + 0.25 * rng.standard_normal((vectors, dim))).astype(np.float32).tolist() for r in range(n)]
    return rows, centers


def test_nanolance_ivf_pq_recall_within_pylance(lance, tmp_path):
    """IVF_PQ over 2,000 rows of 8 vectors: recall@10 of nanolance's index against pylance's, the exact
    answer as truth, the mean over three seeded builds each; the two means differ by at most 0.03."""
    rng = np.random.default_rng(101)
    n, vectors, dim = 2000, 8, 32
    rows, centers = _topic_rows(rng, n, vectors, dim)
    queries = [(centers[rng.integers(0, n, 4)] + 0.25 * rng.standard_normal((4, dim))).astype(np.float32)
               for _ in range(20)]
    src = str(tmp_path / "recall_src.lance")
    nl.write_dataset(_table(rows, pa.float32(), dim), src)
    truth = [set(nl.dataset(src).to_table(columns=["id"], nearest={
        "column": "vec", "q": q, "k": 10, "metric": "cosine", "use_index": False})["id"].to_pylist())
        for q in queries]

    def mean_recall(engine, path):
        hits = 0.0
        for q, gt in zip(queries, truth):
            got = engine.dataset(path).to_table(columns=["id"], nearest={
                "column": "vec", "q": q, "k": 10, "metric": "cosine", "nprobes": 16, "refine_factor": 2})
            hits += len(set(got["id"].to_pylist()) & gt) / 10
        return hits / len(queries)

    means = {}
    for name, engine in (("nanolance", nl), ("pylance", lance)):
        scores = []
        for seed in (1, 2, 3):
            path = str(tmp_path / f"recall_{name}_{seed}.lance")
            engine.write_dataset(_table(rows, pa.float32(), dim), path)
            engine.dataset(path).create_index("vec", index_type="IVF_PQ", metric="cosine", num_partitions=16,
                                              num_sub_vectors=8, seed=seed)
            scores.append(mean_recall(engine, path))
        means[name] = float(np.mean(scores))
        assert means[name] > 0.2  # the test measures something: random answers would score about 0.005
    assert abs(means["nanolance"] - means["pylance"]) <= 0.03, means
