"""IVF_RQ (RaBitQ): ``nearest=`` answered from pylance-built default IVF_RQ indexes (L2, fast rotation,
5 bits), against pylance's own answers: the same rows in the same order, distances within 1e-5 relative.

Phase R1a (search). Building IVF_RQ (R2) and the other rotations and bit counts (R1b) are not here.
"""

from __future__ import annotations

import shutil

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

pytestmark = pytest.mark.filterwarnings("ignore::DeprecationWarning")

N = 3000
SEEDS = (0, 5, 11)
DIMS = (64, 128)
PARTS = (2, 16)


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _vectors(dim, seed):
    rng = np.random.default_rng(seed)
    return rng.standard_normal((N, dim)).astype(np.float32)


def _table(v, dim):
    ids = np.arange(len(v))
    vec = pa.FixedSizeListArray.from_arrays(pa.array(v.ravel()), dim)
    return pa.table({"id": pa.array(ids, pa.int64()), "g": pa.array(ids % 7), "vec": vec})


def _queries(v, seed):
    rng = np.random.default_rng(seed + 1000)
    # One query next to a row, one well away from every row.
    return [v[7] + 0.1, v[1500] + 0.5 * rng.standard_normal(v.shape[1]).astype(np.float32)]


@pytest.fixture(scope="module")
def built(lance, tmp_path_factory):
    """One pylance-built IVF_RQ index per (seed, dim, partitions)."""
    root = tmp_path_factory.mktemp("ivf_rq")
    out = {}
    for seed in SEEDS:
        for dim in DIMS:
            v = _vectors(dim, seed)
            for parts in PARTS:
                path = str(root / f"rq_{seed}_{dim}_{parts}.lance")
                lance.write_dataset(_table(v, dim), path)
                lance.dataset(path).create_index("vec", "IVF_RQ", num_partitions=parts)
                out[seed, dim, parts] = path
    return out


def _same(ref, got, dim):
    """Rows in pylance's order with pylance's distances. A RaBitQ estimate is a difference of terms on the
    scale of the query-to-centroid distance (about 2 * dim for these standard normal vectors), so float32
    rounding leaves an absolute error on that scale, not on each distance's (measured: up to 2.2e-5 next to
    a row at dim 128, 9e-5 where the results reach 280, a few float32 ulps of the terms): distances agree
    rank by rank to 1e-6 of max(2 * dim, the largest distance), and rows may trade places only with a row
    whose distance is that close (a near-tie, which Lance's float order breaks its own way)."""
    rd = ref.column("_distance").to_numpy()
    gd = got.column("_distance").to_numpy()
    rids, gids = ref.column("id").to_pylist(), got.column("id").to_pylist()
    assert len(rids) == len(gids), (rids, gids)
    if not rids:
        return
    tol = 1e-6 * max(2.0 * dim, float(np.abs(rd).max()))
    assert np.allclose(gd, rd, rtol=0, atol=tol), (rd[:5].tolist(), gd[:5].tolist())
    near = 2 * tol
    for i, (r, g) in enumerate(zip(rids, gids)):
        if r != g:  # a swap: only among rows whose distances tie to within `near`
            assert g in rids and r in gids or abs(rd[i] - rd[-1]) <= near, (i, r, g)
            j = rids.index(g) if g in rids else i
            assert abs(rd[j] - rd[i]) <= near, (i, r, g, rd[i], rd[j])


@pytest.mark.parametrize("seed", SEEDS)
@pytest.mark.parametrize("dim", DIMS)
@pytest.mark.parametrize("parts", PARTS)
def test_matches_pylance_from_its_index(lance, built, seed, dim, parts):
    path = built[seed, dim, parts]
    for q in _queries(_vectors(dim, seed), seed):
        for k in (1, 10, 100):
            for nprobes in sorted({1, 2, parts}):
                for refine in (None, 4):
                    extra = {"k": k, "nprobes": nprobes}
                    if refine:
                        extra["refine_factor"] = refine
                    nearest = {"column": "vec", "q": q, **extra}
                    ref = lance.dataset(path).to_table(nearest=nearest, columns=["id"])
                    got = nl.dataset(path).to_table(nearest=nearest, columns=["id"])
                    _same(ref, got, dim)


@pytest.mark.parametrize("prefilter", [False, True])
@pytest.mark.parametrize("flt", ["g = 3", "id < 500", "g = 3 AND id > 2500", "id = 7"])
@pytest.mark.parametrize("key", [(0, 64, 2), (5, 128, 16)])
def test_filters_before_and_after_the_search(lance, built, key, flt, prefilter):
    path = built[key]
    q = _queries(_vectors(key[1], key[0]), key[0])[0]
    for k in (10, 100):
        nearest = {"column": "vec", "q": q, "k": k}
        ref = lance.dataset(path).to_table(nearest=nearest, filter=flt, prefilter=prefilter, columns=["id", "g"])
        got = nl.dataset(path).to_table(nearest=nearest, filter=flt, prefilter=prefilter, columns=["id", "g"])
        _same(ref, got, key[1])


@pytest.mark.parametrize("key", [(5, 128, 16), (0, 64, 2)])
def test_deleted_rows(lance, built, tmp_path, key):
    path = str(tmp_path / "deleted.lance")
    shutil.copytree(built[key], path)
    v = _vectors(key[1], key[0])
    q = v[42] + 0.1
    # The nearest rows deleted: the search goes on to other rows, and partitions.
    lance.dataset(path).delete("id IN (42, 1, 2, 3) OR g = 2")
    for k in (1, 10, 100):
        for nprobes in sorted({1, key[2]}):
            for refine in (None, 4):
                extra = {"k": k, "nprobes": nprobes}
                if refine:
                    extra["refine_factor"] = refine
                nearest = {"column": "vec", "q": q, **extra}
                ref = lance.dataset(path).to_table(nearest=nearest, columns=["id"])
                got = nl.dataset(path).to_table(nearest=nearest, columns=["id"])
                _same(ref, got, key[1])
