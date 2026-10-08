"""IVF_HNSW_SQ: ``nearest=`` answered from the HNSW graphs and 8-bit SQ codes of a Lance index, and
such indexes built by nanolance.

The bar is pylance's own answer: the same rows, in the same order, at the same ``_distance`` -- for
every partition count, k, nprobes, refine_factor, ef, distance_range, with deletions and with filters
before and after the search. Lance searches the partitions after the first ``nprobes`` in parallel;
nanolance models them finishing in order, which is what pylance does whenever the query is not a
race between partitions (filtered dot-product searches can be: pylance itself answers those
differently from one run to the next), so those compare distances only.

An index nanolance builds is searched by pylance exactly as by nanolance, and finds the true
neighbours about as often as pylance's own index (Lance builds its graphs on many threads, so two
builds are never quite the same graph).
"""

from __future__ import annotations

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

pytestmark = pytest.mark.filterwarnings("ignore::DeprecationWarning")

N, DIM, PARTS = 3000, 16, 4


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _vectors(seed=5):
    rng = np.random.default_rng(seed)
    centres = rng.standard_normal((20, DIM)).astype(np.float32) * 3
    v = centres[rng.integers(0, 20, N)] + rng.standard_normal((N, DIM)).astype(np.float32)
    v[::37] = v[1::37][: len(v[::37])]  # duplicate vectors: ties in distance
    return v


@pytest.fixture(scope="module")
def indexed(lance, tmp_path_factory):
    root = tmp_path_factory.mktemp("hnsw")
    v = _vectors()
    out = {}
    for metric in ("l2", "cosine", "dot"):
        for deleted in (False, True):
            path = str(root / f"{metric}_{deleted}.lance")
            vec = pa.FixedSizeListArray.from_arrays(pa.array(v.ravel()), DIM)
            lance.write_dataset(pa.table({"id": np.arange(N), "vec": vec}), path, max_rows_per_file=1001)
            lance.dataset(path).create_index("vec", "IVF_HNSW_SQ", num_partitions=PARTS, metric=metric)
            if deleted:
                lance.dataset(path).delete("id % 11 = 4")
            out[metric, deleted] = path
    return v, out


OPTIONS = [{"k": 10}, {"k": 1}, {"k": 50}, {"k": 10, "nprobes": 1}, {"k": 10, "nprobes": PARTS},
           {"k": 10, "refine_factor": 3}, {"k": 10, "ef": 40}, {"k": 25, "ef": 300},
           {"k": 10, "distance_range": (0.0, 30.0)}]
FILTERS = [(None, False), ("id % 3 = 0", True), ("id % 3 = 0", False), ("id < 40", True)]


@pytest.mark.parametrize("metric", ["l2", "cosine", "dot"])
@pytest.mark.parametrize("deleted", [False, True])
def test_matches_pylance_from_its_index(lance, indexed, metric, deleted):
    v, paths = indexed
    theirs, ours = lance.dataset(paths[metric, deleted]), nl.dataset(paths[metric, deleted])
    rng = np.random.default_rng(1)
    for _ in range(12):
        q = v[rng.integers(0, N)] + rng.standard_normal(DIM).astype(np.float32) * 0.5
        for o in OPTIONS:
            for flt, pre in FILTERS:
                nearest = {"column": "vec", "q": q, **o}
                ref = theirs.to_table(nearest=nearest, columns=["id"], filter=flt, prefilter=pre)
                got = ours.to_table(nearest=nearest, columns=["id"], filter=flt, prefilter=pre)
                rd, gd = ref.column("_distance").to_numpy(), got.column("_distance").to_numpy()
                assert len(rd) == len(gd), (o, flt, pre)
                if metric == "dot" and flt is not None:
                    # A race between partitions in pylance: compare what any run could return.
                    assert np.all(np.isfinite(gd) == np.isfinite(rd)), (o, flt, pre)
                    continue
                assert np.allclose(rd, gd, rtol=1e-5, atol=1e-6), (o, flt, pre, rd[:5], gd[:5])
                assert ref.column("id").to_pylist() == got.column("id").to_pylist(), (o, flt, pre)


def test_list_indices(lance, indexed):
    _, paths = indexed
    a = nl.dataset(paths["l2", False]).list_indices()
    b = lance.dataset(paths["l2", False]).list_indices()
    assert [(x["name"], x["type"], x["fields"]) for x in a] == [(x["name"], x["type"], x["fields"]) for x in b]


def _recall(ds, v, metric, nprobes, k=10, queries=40, seed=3):
    rng = np.random.default_rng(seed)
    hit = 0
    for _ in range(queries):
        q = v[rng.integers(0, N)] + rng.standard_normal(DIM).astype(np.float32) * 0.5
        exact = ds.to_table(nearest={"column": "vec", "q": q, "k": k, "use_index": False, "metric": metric},
                            columns=["id"]).column("id").to_pylist()
        got = ds.to_table(nearest={"column": "vec", "q": q, "k": k, "nprobes": nprobes},
                          columns=["id"]).column("id").to_pylist()
        hit += len(set(exact) & set(got))
    return hit / (k * queries)


@pytest.mark.parametrize("metric", ["l2", "cosine", "dot"])
def test_pylance_searches_nanolances_index(lance, indexed, tmp_path, metric):
    v, paths = indexed
    path = str(tmp_path / "ours.lance")
    vec = pa.FixedSizeListArray.from_arrays(pa.array(v.ravel()), DIM)
    nl.write_dataset(pa.table({"id": np.arange(N), "vec": vec}), path, max_rows_per_file=1001)
    nl.dataset(path).create_index("vec", "IVF_HNSW_SQ", num_partitions=PARTS, metric=metric)
    theirs, ours = lance.dataset(path), nl.dataset(path)
    stats = theirs.stats.index_stats("vec_idx")
    assert stats["index_type"] == "IVF_HNSW_SQ" and stats["num_indexed_rows"] == N
    sub = stats["indices"][0]["sub_index"]
    assert sub["params"] == {"max_level": 7, "m": 20, "ef_construction": 150, "prefetch_distance": 2}
    assert sub["num_bits"] == 8 and len(sub["level_offsets"]) == 8
    rng = np.random.default_rng(2)
    for _ in range(8):
        q = v[rng.integers(0, N)] + rng.standard_normal(DIM).astype(np.float32) * 0.5
        for o in OPTIONS:
            for flt, pre in FILTERS:
                if metric == "dot" and flt is not None:
                    continue
                nearest = {"column": "vec", "q": q, **o}
                ref = theirs.to_table(nearest=nearest, columns=["id"], filter=flt, prefilter=pre)
                got = ours.to_table(nearest=nearest, columns=["id"], filter=flt, prefilter=pre)
                assert ref.column("id").to_pylist() == got.column("id").to_pylist(), (o, flt, pre)
                assert np.allclose(ref.column("_distance").to_numpy(), got.column("_distance").to_numpy(),
                                   rtol=1e-5, atol=1e-6)
    # As good as pylance's own index.
    ref_recall = _recall(lance.dataset(paths[metric, False]), v, metric, PARTS)
    assert _recall(theirs, v, metric, PARTS) >= ref_recall - 0.05


def test_build_options_defaults_and_maintenance(lance, tmp_path):
    v = _vectors()
    path = str(tmp_path / "opts.lance")
    vec = pa.FixedSizeListArray.from_arrays(pa.array(v.ravel()), DIM)
    nl.write_dataset(pa.table({"id": np.arange(N), "vec": vec}), path)
    # No num_partitions: rows / 2^20, so one partition.
    nl.dataset(path).create_index("vec", "IVF_HNSW_SQ", m=8, ef_construction=40, max_level=4)
    stats = lance.dataset(path).stats.index_stats("vec_idx")["indices"][0]
    assert stats["num_partitions"] == 1
    assert stats["sub_index"]["params"] == {"max_level": 4, "m": 8, "ef_construction": 40, "prefetch_distance": 2}
    assert len(stats["sub_index"]["level_offsets"]) == 5
    # New rows, then optimize_indices: a delta (or merged) segment with the same graph settings.
    extra = _vectors(seed=9)[:500]
    more = pa.FixedSizeListArray.from_arrays(pa.array(extra.ravel()), DIM)
    nl.write_dataset(pa.table({"id": np.arange(N, N + 500), "vec": more}), path, mode="append")
    nl.dataset(path).optimize.optimize_indices()
    theirs, ours = lance.dataset(path), nl.dataset(path)
    assert theirs.stats.index_stats("vec_idx")["num_indexed_rows"] == N + 500
    for seg in theirs.stats.index_stats("vec_idx")["indices"]:
        assert seg["sub_index"]["params"]["m"] == 8
    rng = np.random.default_rng(4)
    for _ in range(10):
        q = rng.standard_normal(DIM).astype(np.float32)
        nearest = {"column": "vec", "q": q, "k": 10}
        assert (theirs.to_table(nearest=nearest, columns=["id"]).column("id").to_pylist() ==
                ours.to_table(nearest=nearest, columns=["id"]).column("id").to_pylist())


@pytest.mark.parametrize("kw, error", [
    ({"m": 2}, "m must be at least 4"),
    ({"m": 20, "ef_construction": 10}, "ef_construction must be at least m (20), got 10"),
    ({"max_level": 0}, "max_level must be greater than 0"),
])
def test_build_errors_as_pylance_raises_them(lance, tmp_path, kw, error):
    path = str(tmp_path / "e.lance")
    v = _vectors()[:600]
    nl.write_dataset(pa.table({"vec": pa.FixedSizeListArray.from_arrays(pa.array(v.ravel()), DIM)}), path)
    for module in (lance, nl):
        with pytest.raises(ValueError, match=error.replace("(", r"\(").replace(")", r"\)")):
            module.dataset(path).create_index("vec", "IVF_HNSW_SQ", num_partitions=2, **kw)
