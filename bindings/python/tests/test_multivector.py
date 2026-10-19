"""Multivector search (ColBERT-style: a list of fixed-size lists): flat search, the bar being pylance's answer.

A row's distance sums, over the query's vectors, the least distance to any of the row's vectors. Rows
with a null or empty vector list are left out, as Lance leaves them out. Over a float32 IVF_FLAT index the
search is Lance's XTR (the candidates per query vector, then the sum; see the IVF_FLAT tests at the end);
other index kinds are left unused, and their search is not written yet.
"""

from __future__ import annotations

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


def test_fast_search_over_an_ivf_sq_multivector_index_is_refused(lance, tmp_path):
    """Only IVF_FLAT is searched over a multivector column so far: fast_search on another kind is refused."""
    vt = pa.list_(pa.list_(pa.float32(), 2))
    path = str(tmp_path / "fast.lance")
    lance.write_dataset(pa.table({"id": pa.array([0, 1], pa.int32()),
                                  "vector": pa.array([[[1.0, 0.0]], [[0.0, 1.0]]], type=vt)}), path)
    lance.dataset(path).create_index("vector", index_type="IVF_SQ", metric="cosine", num_partitions=1)
    nearest = {"column": "vector", "q": [[1.0, 0.0]], "k": 2, "metric": "cosine"}
    with pytest.raises(nl.NotSupportedError, match="multivector"):
        nl.dataset(path).to_table(columns=["id"], nearest=nearest, fast_search=True)


def test_refine_factor_over_a_multivector_ivf_flat_index_is_refused(lance, tmp_path):
    """Refine on a multivector index needs its own answer (not written yet): refused, not answered wrongly."""
    vt = pa.list_(pa.list_(pa.float32(), 2))
    path = str(tmp_path / "refine.lance")
    lance.write_dataset(pa.table({"id": pa.array([0, 1], pa.int32()),
                                  "vector": pa.array([[[1.0, 0.0]], [[0.0, 1.0]]], type=vt)}), path)
    lance.dataset(path).create_index("vector", index_type="IVF_FLAT", metric="cosine", num_partitions=1)
    nearest = {"column": "vector", "q": [[1.0, 0.0]], "k": 2, "metric": "cosine", "refine_factor": 2}
    with pytest.raises(nl.NotSupportedError, match="refine_factor"):
        nl.dataset(path).to_table(columns=["id"], nearest=nearest)


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


# -- IVF_FLAT over a multivector column: Lance's XTR (float32; pylance builds these with cosine only) --


def _dense(rng, n):
    """Rows of 1 to 4 vectors each, none null or empty: every row has vectors in the index."""
    return [rng.standard_normal((int(rng.integers(1, 5)), DIM)).astype(np.float32).tolist() for _ in range(n)]


def _ivf_flat_dataset(lance, tmp_path, name, rows, indexed, parts):
    """``rows`` written by pylance: the first ``indexed`` under a cosine IVF_FLAT index, the rest appended."""
    path = str(tmp_path / f"{name}.lance")
    vt = pa.list_(pa.list_(pa.float32(), DIM))

    def table(lo, hi):
        return pa.table({"id": pa.array(np.arange(lo, hi), pa.int64()), "vec": pa.array(rows[lo:hi], type=vt)})

    lance.write_dataset(table(0, indexed), path)
    lance.dataset(path).create_index("vec", index_type="IVF_FLAT", metric="cosine", num_partitions=parts)
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
