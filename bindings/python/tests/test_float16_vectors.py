"""Searches over float16 vector columns answer with pylance's distances.

pylance casts the query to the column's element type (a float16 column's query is rounded), and a
cosine index normalizes that query and rounds it to float16 again; its PQ residuals are float16 too.
Measured to 1e-7 against pylance 12.0.0; nanolance does the same. Distances agree to 1e-5 relative (the
rest is float32 summation order: Lance's float16 kernels are SIMD with FMA); before this they differed by
about 5e-5, enough to reorder rows and to send an HNSW search down another path.
"""

import numpy as np
import pyarrow as pa
import pytest

from support import require_pylance

import nanolance.lance as nl

KINDS = [("IVF_FLAT", {}), ("IVF_PQ", {"num_sub_vectors": 4}), ("IVF_SQ", {}),
         ("IVF_HNSW_FLAT", {}), ("IVF_HNSW_SQ", {}), ("IVF_HNSW_PQ", {"num_sub_vectors": 4})]


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _dataset(lance, path, rng, n=600, dim=32):
    v = rng.standard_normal((n, dim)).astype(np.float16)
    lance.write_dataset(pa.table({"id": np.arange(n),
                                  "vec": pa.FixedSizeListArray.from_arrays(pa.array(v.ravel(), pa.float16()), dim)}),
                        path)
    return v


@pytest.mark.parametrize("metric", ["cosine", "l2", "dot"])
def test_exact_search(lance, tmp_path, metric):
    rng = np.random.default_rng(1)
    path = str(tmp_path / "f16.lance")
    _dataset(lance, path, rng)
    q = rng.standard_normal(32).astype(np.float32)
    nearest = {"column": "vec", "q": q, "k": 10, "metric": metric, "use_index": False}
    ref = lance.dataset(path).to_table(nearest=nearest, columns=["id"])
    got = nl.dataset(path).to_table(nearest=nearest, columns=["id"])
    assert got["id"].to_pylist() == ref["id"].to_pylist()
    np.testing.assert_allclose(got["_distance"].to_numpy(), ref["_distance"].to_numpy(), rtol=1e-6, atol=1e-6)


@pytest.mark.parametrize("kind, kw", KINDS)
@pytest.mark.parametrize("metric", ["cosine", "l2"])
def test_pylance_built_index(lance, tmp_path, kind, kw, metric):
    rng = np.random.default_rng(2)
    path = str(tmp_path / "f16.lance")
    _dataset(lance, path, rng)
    lance.dataset(path).create_index("vec", kind, num_partitions=2, metric=metric, **kw)
    for seed in range(3):
        q = np.random.default_rng(100 + seed).standard_normal(32).astype(np.float32)
        nearest = {"column": "vec", "q": q, "k": 10, "nprobes": 2}
        ref = lance.dataset(path).to_table(nearest=nearest, columns=["id"])
        got = nl.dataset(path).to_table(nearest=nearest, columns=["id"])
        assert got["id"].to_pylist() == ref["id"].to_pylist(), (seed, got["id"].to_pylist(), ref["id"].to_pylist())
        # PQ sums many table entries, so its float32 order leaves a little more (measured up to 1.2e-5)
        rtol = 5e-5 if kind.endswith("PQ") else 1e-5
        np.testing.assert_allclose(got["_distance"].to_numpy(), ref["_distance"].to_numpy(), rtol=rtol, atol=1e-6)
