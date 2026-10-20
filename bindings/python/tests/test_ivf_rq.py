"""IVF_RQ (RaBitQ): ``nearest=`` answered from pylance-built IVF_RQ indexes, against pylance's own answers:
the same rows in the same order, distances within the tolerance of ``_same``.

Phase R1a: the default index (L2, fast rotation, 5 bits). Phase R1b: every num_bits pylance accepts (1 to 9),
cosine and dot, skip_transpose, and index_stats. Phase R1c: approx_mode 'fast' and 'accurate'. Phase R2: nanolance builds IVF_RQ indexes that pylance searches
with nanolance's own answers, the recall bound, seeded and shared-model builds, and the refusals.
"""

from __future__ import annotations

import json
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
    worst = int(np.argmax(np.abs(gd - rd)))
    assert np.allclose(gd, rd, rtol=0, atol=tol), (worst, rids[worst], gids[worst], float(rd[worst]), float(gd[worst]), tol)
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


# ── R1b: num_bits, cosine and dot, skip_transpose, index statistics ─────────────────────────────

@pytest.fixture(scope="module")
def variants(lance, tmp_path_factory):
    """pylance-built IVF_RQ indexes by (seed, dim, partitions, num_bits, metric, skip_transpose)."""
    root = tmp_path_factory.mktemp("ivf_rq_r1b")
    cache = {}

    def get(seed, dim, parts, num_bits=5, metric=None, skip=False):
        key = (seed, dim, parts, num_bits, metric, skip)
        if key not in cache:
            path = str(root / ("rq_" + "_".join(str(x) for x in key) + ".lance"))
            lance.write_dataset(_table(_vectors(dim, seed), dim), path)
            kwargs = {"num_bits": num_bits, "skip_transpose": skip}
            if metric is not None:
                kwargs["metric"] = metric
            lance.dataset(path).create_index("vec", "IVF_RQ", num_partitions=parts, **kwargs)
            cache[key] = path
        return cache[key]

    return get


def _compare_all(lance, path, dim, parts, queries, ks=(1, 10, 100), approx_mode=None):
    """Every k, nprobes (1, 2, all) and refine (none, 4) for each query, both engines on one index."""
    for q in queries:
        for k in ks:
            for nprobes in sorted({1, min(2, parts), parts}):
                for refine in (None, 4):
                    extra = {"k": k, "nprobes": nprobes}
                    if refine:
                        extra["refine_factor"] = refine
                    if approx_mode:
                        extra["approx_mode"] = approx_mode
                    nearest = {"column": "vec", "q": q, **extra}
                    ref = lance.dataset(path).to_table(nearest=nearest, columns=["id"])
                    got = nl.dataset(path).to_table(nearest=nearest, columns=["id"])
                    _same(ref, got, dim)


@pytest.mark.parametrize("num_bits", list(range(1, 10)))
def test_num_bits_matches_pylance(lance, variants, num_bits):
    """num_bits 1 (no extended codes, no top-k gate) to 9 (8 extended bits), dim 64, 2 partitions."""
    path = variants(0, 64, 2, num_bits=num_bits)
    _compare_all(lance, path, 64, 2, _queries(_vectors(64, 0), 0))


@pytest.mark.parametrize("num_bits", [1, 2, 3, 6, 7, 8, 9])
def test_num_bits_dim128_many_partitions(lance, variants, num_bits):
    """The extended layouts for the other widths at dim 128 (two 64-dim blocks), 16 partitions."""
    path = variants(5, 128, 16, num_bits=num_bits)
    _compare_all(lance, path, 128, 16, _queries(_vectors(128, 5), 5), ks=(10, 100))


@pytest.mark.parametrize("metric", ["cosine", "dot"])
@pytest.mark.parametrize("num_bits", [1, 5, 9])
@pytest.mark.parametrize("key", [(0, 64, 2), (5, 128, 16)])
def test_cosine_and_dot_match_pylance(lance, variants, metric, num_bits, key):
    seed, dim, parts = key
    path = variants(seed, dim, parts, num_bits=num_bits, metric=metric)
    _compare_all(lance, path, dim, parts, _queries(_vectors(dim, seed), seed))


@pytest.mark.parametrize("num_bits", [1, 5, 9])
@pytest.mark.parametrize("key", [(0, 64, 2), (5, 128, 16)])
def test_skip_transpose_matches_pylance(lance, variants, num_bits, key):
    """skip_transpose keeps the binary codes row-major; Lance packs them at load, so answers are the same."""
    seed, dim, parts = key
    path = variants(seed, dim, parts, num_bits=num_bits, skip=True)
    _compare_all(lance, path, dim, parts, _queries(_vectors(dim, seed), seed))


@pytest.mark.parametrize("flt", ["g = 3", "id < 500"])
@pytest.mark.parametrize("num_bits", [1, 9])
def test_filters_and_deletions_for_other_widths(lance, variants, tmp_path, num_bits, flt):
    path = variants(0, 64, 2, num_bits=num_bits, metric="cosine")
    q = _queries(_vectors(64, 0), 0)[0]
    for k in (10, 100):
        nearest = {"column": "vec", "q": q, "k": k}
        ref = lance.dataset(path).to_table(nearest=nearest, filter=flt, prefilter=True, columns=["id", "g"])
        got = nl.dataset(path).to_table(nearest=nearest, filter=flt, prefilter=True, columns=["id", "g"])
        _same(ref, got, 64)
    copy = str(tmp_path / "deleted.lance")
    shutil.copytree(path, copy)
    lance.dataset(copy).delete("id IN (7, 8, 9) OR g = 2")
    nearest = {"column": "vec", "q": q, "k": 100, "nprobes": 2}
    _same(lance.dataset(copy).to_table(nearest=nearest, columns=["id"]),
          nl.dataset(copy).to_table(nearest=nearest, columns=["id"]), 64)


@pytest.mark.parametrize("mode", ["fast", "normal"])
@pytest.mark.parametrize("num_bits", [1, 9])
def test_default_nprobes_with_deletions(lance, variants, tmp_path, num_bits, mode):
    """With no nprobes given, Lance probes the partitions its first-pass rule picks and then more only while
    fewer than k rows were found; it does not probe them all. The nearest row deleted makes that visible at
    k=1: the first partition still holds a live row, which pylance answers with."""
    path = variants(5, 128, 16, num_bits=num_bits)
    copy = str(tmp_path / "deleted.lance")
    shutil.copytree(path, copy)
    lance.dataset(copy).delete("id IN (7, 8, 9) OR g = 2")
    for q in _queries(_vectors(128, 5), 5):
        for k in (1, 2, 10, 100):
            nearest = {"column": "vec", "q": q, "k": k, "approx_mode": mode}
            _same(lance.dataset(copy).to_table(nearest=nearest, columns=["id"]),
                  nl.dataset(copy).to_table(nearest=nearest, columns=["id"]), 128)


@pytest.mark.parametrize("num_bits, metric, skip", [(1, None, False), (5, "cosine", False), (9, "dot", True)])
def test_index_stats_match_pylance(lance, variants, num_bits, metric, skip):
    """stats.index_stats for IVF_RQ: the same dict as pylance reports (sub_index: rotation, code_dim,
    num_bits, packed, query_estimator, index_type RQ, the sign bits)."""
    path = variants(0, 64, 2, num_bits=num_bits, metric=metric, skip=skip)
    name = lance.dataset(path).list_indices()[0]["name"]
    ref = lance.dataset(path).stats.index_stats(name)
    got = nl.dataset(path).stats.index_stats(name)
    assert got == ref
    sub = got["indices"][0]["sub_index"]
    assert sub["num_bits"] == num_bits
    assert sub["packed"] is (not skip)
    assert sub["query_estimator"] == "raw_query"
    assert sub["rotation_type"] == "fast"


# ── R1c: approx_mode 'fast' and 'accurate' ──────────────────────────────────────────────────────────

@pytest.mark.parametrize("mode", ["fast", "accurate"])
@pytest.mark.parametrize("metric", ["l2", "cosine", "dot"])
@pytest.mark.parametrize("dim", DIMS)
@pytest.mark.parametrize("num_bits", [1, 5, 9])
def test_approx_modes_match_pylance(lance, variants, mode, num_bits, dim, metric):
    """pylance's 'fast' (1-bit scores, no top-k gate) and 'accurate' (16-bit binary table) answers, for
    num_bits 1 (no gate; 'fast' is then the normal answer), 5 and 9, both dims, every metric."""
    path = variants(0, dim, 4, num_bits=num_bits, metric=None if metric == "l2" else metric)
    _compare_all(lance, path, dim, 4, _queries(_vectors(dim, 0), 0), approx_mode=mode)


@pytest.mark.parametrize("mode", ["fast", "accurate"])
@pytest.mark.parametrize("flt", ["g = 3", "id < 500"])
@pytest.mark.parametrize("prefilter", [False, True])
def test_approx_modes_with_filters_and_deletions(lance, variants, tmp_path, mode, flt, prefilter):
    path = variants(5, 128, 16, num_bits=9)
    q = _queries(_vectors(128, 5), 5)[0]
    for k in (10, 100):
        nearest = {"column": "vec", "q": q, "k": k, "nprobes": 16, "approx_mode": mode}
        ref = lance.dataset(path).to_table(nearest=nearest, filter=flt, prefilter=prefilter, columns=["id", "g"])
        got = nl.dataset(path).to_table(nearest=nearest, filter=flt, prefilter=prefilter, columns=["id", "g"])
        _same(ref, got, 128)
    copy = str(tmp_path / "approx_deleted.lance")
    shutil.copytree(path, copy)
    lance.dataset(copy).delete("id IN (7, 8, 9) OR g = 2")
    # Explicit nprobes: pylance's adaptive count (minimum_nprobes 1, no maximum) differs from nanolance's
    # once rows are deleted (see the R1c outcome in docs/tasks/IVF_RQ.md), in every mode, not only these.
    for k in (1, 10, 100):
        for nprobes in (1, 16):
            nearest = {"column": "vec", "q": q, "k": k, "nprobes": nprobes, "approx_mode": mode}
            ref = lance.dataset(copy).to_table(nearest=nearest, columns=["id"])
            got = nl.dataset(copy).to_table(nearest=nearest, columns=["id"])
            _same(ref, got, 128)


@pytest.mark.parametrize("mode", ["fast", "accurate", "normal"])
def test_approx_mode_is_ignored_by_other_kinds(lance, tmp_path, mode):
    """IVF_PQ: approx_mode changes nothing, as in pylance."""
    v = _vectors(64, 0)
    path = str(tmp_path / "pq.lance")
    lance.write_dataset(_table(v, 64), path)
    lance.dataset(path).create_index("vec", "IVF_PQ", num_partitions=4, num_sub_vectors=8)
    for q in _queries(v, 0):
        nearest = {"column": "vec", "q": q, "k": 10, "approx_mode": mode}
        ref = lance.dataset(path).to_table(nearest=nearest, columns=["id"])
        got = nl.dataset(path).to_table(nearest=nearest, columns=["id"])
        _same(ref, got, 64)


def test_approx_mode_rejects_unknown_names(built):
    path = built[SEEDS[0], DIMS[0], PARTS[0]]
    q = _queries(_vectors(DIMS[0], SEEDS[0]), SEEDS[0])[0]
    with pytest.raises(ValueError, match="approx_mode"):
        nl.dataset(path).to_table(nearest={"column": "vec", "q": q, "k": 5, "approx_mode": "turbo"})


# ── R2: nanolance builds IVF_RQ ─────────────────────────────────────────────────────────────────────────────

BUILD_CASES = ([(bits, "l2", False) for bits in range(1, 10)]
               + [(bits, metric, False) for metric in ("cosine", "dot") for bits in (1, 5, 9)]
               + [(1, "l2", True), (5, "dot", True), (9, "cosine", True)])


def _nano_build(tmp_path, dim, parts, num_bits=5, metric=None, skip=False, seed=1, name="rq.lance"):
    v = _vectors(dim, 0)
    path = str(tmp_path / name)
    nl.write_dataset(_table(v, dim), path)
    kwargs = {"num_bits": num_bits, "skip_transpose": skip, "seed": seed}
    if metric is not None:
        kwargs["metric"] = metric
    nl.dataset(path).create_index("vec", "IVF_RQ", num_partitions=parts, **kwargs)
    return path, v


@pytest.mark.parametrize("num_bits,metric,skip", BUILD_CASES)
def test_nanolance_build_searched_by_pylance_as_nanolance(lance, tmp_path, num_bits, metric, skip):
    """A nanolance-built index: pylance's search gives the rows and distances nanolance's search gives."""
    dim, parts = 64, 4
    path, v = _nano_build(tmp_path, dim, parts, num_bits, None if metric == "l2" else metric, skip)
    for q in _queries(v, 0):
        for k in (1, 10):
            for nprobes in (1, parts):
                for refine in (None, 4):
                    nearest = {"column": "vec", "q": q, "k": k, "nprobes": nprobes}
                    if metric != "l2":
                        nearest["metric"] = metric
                    if refine:
                        nearest["refine_factor"] = refine
                    ref = lance.dataset(path).to_table(nearest=nearest, columns=["id"])
                    got = nl.dataset(path).to_table(nearest=nearest, columns=["id"])
                    _same(ref, got, dim)


def test_nanolance_build_index_stats_match_pylance(lance, tmp_path):
    path, _ = _nano_build(tmp_path, 64, 4, num_bits=9, metric="cosine", skip=True)
    assert nl.dataset(path).stats.index_stats("vec_idx") == lance.dataset(path).stats.index_stats("vec_idx")
    sub = nl.dataset(path).stats.index_stats("vec_idx")["indices"][0]["sub_index"]
    assert sub["num_bits"] == 9 and sub["packed"] is False and sub["query_estimator"] == "raw_query"


def test_seeded_build_is_reproducible(tmp_path):
    """Two builds with one seed write the same index files, byte for byte (as the other kinds' seeded builds)."""
    import glob
    import hashlib

    digests = []
    for name in ("a.lance", "b.lance"):
        path, _ = _nano_build(tmp_path, 64, 4, seed=7, name=name)
        files = sorted(glob.glob(path + "/_indices/*/*"))
        digests.append([(f.rsplit("/", 1)[1], hashlib.sha256(open(f, "rb").read()).hexdigest()) for f in files])
    assert digests[0] == digests[1]
    assert len(digests[0]) == 2  # auxiliary.idx and index.idx


def test_recall_within_0_03_of_pylance(lance, tmp_path):
    """20,000 x 128, 64 partitions, mean over 3 builds each: nanolance's index is searched by pylance and its
    recall@10 is within 0.03 of pylance's own index."""
    n, dim, parts, nq = 20000, 128, 64, 100
    rng = np.random.default_rng(0)
    v = rng.standard_normal((n, dim)).astype(np.float32)
    qs = v[rng.choice(n, nq, replace=False)] + 0.3 * rng.standard_normal((nq, dim)).astype(np.float32)
    dist = (qs ** 2).sum(1)[:, None] - 2 * qs @ v.T + (v ** 2).sum(1)[None, :]
    truth = np.argsort(dist, axis=1)[:, :10]
    tbl = _table(v, dim)

    def recall(path):
        hits = 0.0
        for qi, q in enumerate(qs):
            got = lance.dataset(path).to_table(nearest={"column": "vec", "q": q, "k": 10}, columns=["id"])
            hits += len(set(got["id"].to_pylist()) & set(truth[qi].tolist())) / 10
        return hits / nq

    means = {}
    for engine, mod in (("pylance", lance), ("nanolance", nl)):
        values = []
        for build in range(3):
            path = str(tmp_path / f"{engine}_{build}.lance")
            mod.write_dataset(tbl, path)
            mod.dataset(path).create_index("vec", "IVF_RQ", num_partitions=parts, seed=build + 1)
            values.append(recall(path))
        means[engine] = float(np.mean(values))
    assert means["nanolance"] >= means["pylance"] - 0.03, means


def test_build_rq_model_matches_pylance_shape():
    from lance.lance import indices as pylance_indices

    from nanolance.lance.lance import indices as nanolance_indices

    ref = json.loads(pylance_indices.build_rq_model(dimension=32, num_bits=3))
    got = json.loads(nanolance_indices.build_rq_model(dimension=32, num_bits=3))
    assert set(got) == set(ref)
    assert got["code_dim"] == 32 and got["num_bits"] == 3 and got["rotation_type"] == "fast"
    assert got["rotate_mat_position"] is None and len(got["fast_rotation_signs"]) == 4 * 32 // 8
    with pytest.raises(ValueError, match="divisible by 8"):
        nanolance_indices.build_rq_model(dimension=30)


def test_shared_model_segments_merge_and_search_as_pylance(lance, tmp_path):
    """Two segments built over the two fragments with one shared RaBitQ model merge and commit into one index;
    pylance searches it with nanolance's own answers."""
    from nanolance.lance.lance import indices

    dim = 32
    qs = _queries(_vectors(dim, 3), 3)
    v = _vectors(dim, 3)[:512]
    path = str(tmp_path / "seg.lance")
    nl.write_dataset(_table(v[:256], dim), path)
    nl.write_dataset(_table(v[256:], dim).set_column(0, "id", pa.array(np.arange(256, 512), pa.int64())),
                     path, mode="append")
    ds = nl.dataset(path)
    frags = ds.get_fragments()
    assert len(frags) == 2
    from nanolance.lance.indices import IndicesBuilder

    ivf = IndicesBuilder(ds, "vec").train_ivf(num_partitions=2, distance_type="l2", sample_rate=8, max_iters=2)
    model = indices.build_rq_model(dimension=dim, num_bits=1)
    segments = [ds.create_index_uncommitted("vec", "IVF_RQ", num_partitions=2, num_bits=1, rabitq_model=model,
                                            ivf_centroids=ivf.centroids, fragment_ids=[f.fragment_id])
                for f in frags]
    merged = ds.merge_existing_index_segments(segments)
    ds = ds.commit_existing_index_segments("vec_idx", "vec", [merged])
    for q in qs:
        nearest = {"column": "vec", "q": q, "k": 10}
        _same(lance.dataset(path).to_table(nearest=nearest, columns=["id"]),
              nl.dataset(path).to_table(nearest=nearest, columns=["id"]), dim)
    assert ds.to_table(nearest={"column": "vec", "q": qs[0], "k": 10}).num_rows == 10


def test_refuses_what_pylance_refuses(tmp_path):
    path, _ = _nano_build(tmp_path, 64, 4)
    with pytest.raises(ValueError, match="vector dimension must be divisible by 8 for IVF_RQ"):
        nl.write_dataset(_table(np.zeros((300, 30), dtype=np.float32), 30), str(tmp_path / "d30.lance"))
        nl.dataset(str(tmp_path / "d30.lance")).create_index("vec", "IVF_RQ", num_partitions=4, num_bits=1)
    with pytest.raises(ValueError, match="num_bits must be in 1..=9"):
        nl.dataset(path).create_index("vec", "IVF_RQ", num_partitions=4, num_bits=10, replace=True)
