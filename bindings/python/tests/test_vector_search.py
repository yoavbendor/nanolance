"""Vector search: ``nearest=`` answered from Lance's IVF_FLAT / IVF_PQ indexes, or exactly.

The bar is pylance's own answer for the same dataset and query: the same rows in the same order with
the same ``_distance`` (to float tolerance) -- for an index pylance built, for the fragments it does
not cover, with filters before or after the search, deletions, and without an index. 4-bit PQ ranks
by quantized distances, so rows of equal distance may come in another order there.
"""

from __future__ import annotations

import shutil

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
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


def _same(ref, got, four_bit=False):
    rd, gd = ref.column("_distance").to_numpy(), got.column("_distance").to_numpy()
    assert len(rd) == len(gd)
    assert np.allclose(rd, gd, rtol=1e-4, atol=1e-4), (rd[:5], gd[:5])
    if not four_bit:
        assert ref.column("id").to_pylist() == got.column("id").to_pylist()
    else:
        assert np.allclose(np.sort(rd), np.sort(gd), rtol=1e-4, atol=1e-4)


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
            _same(ref, got, four_bit=key[2] == 4)


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
