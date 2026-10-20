"""Binary vectors (a fixed-size list of uint8) in flat nearest() search, and
lance.vector.hamming_clustering_for_sample, checked against pylance: the same files written by both
engines, the same query, the same answers. The IVF index on such a column stays refused."""

from __future__ import annotations

import importlib

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance.vector import hamming_clustering_for_sample as nl_clustering
from tests.support import require_pylance

POPCOUNT = np.array([bin(i).count("1") for i in range(256)], dtype=np.int64)


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def binary_table(rows, width, nulls=()):
    """``rows``: an (n, width) uint8 array; ``nulls``: row numbers whose vector is null."""
    values = pa.array(np.asarray(rows, dtype=np.uint8).ravel(), type=pa.uint8())
    mask = None
    if nulls:
        mask = pa.array([i in nulls for i in range(len(rows))])
    vectors = pa.FixedSizeListArray.from_arrays(values, list_size=width, mask=mask)
    ids = pa.array(range(len(rows)), type=pa.int64())
    return pa.Table.from_arrays([ids, vectors], names=["id", "vector"])


def oracle(rows, query):
    """Hamming distance (bits) of each row to the query, the reference both engines must match."""
    xor = np.bitwise_xor(np.asarray(rows, dtype=np.uint8), np.asarray(query, dtype=np.uint8))
    return POPCOUNT[xor].sum(axis=1)


def write_both(lance, tmp_path, table):
    return (lance.write_dataset(table, tmp_path / "pylance"),
            nl.write_dataset(table, str(tmp_path / "nanolance")))


def random_rows(seed, n, width):
    return np.random.default_rng(seed).integers(0, 256, size=(n, width), dtype=np.uint8)


@pytest.mark.parametrize("width", [4, 8, 16, 32])
def test_default_hamming_matches_pylance(lance, tmp_path, width):
    rows = random_rows(width, 200, width)
    theirs, ours = write_both(lance, tmp_path, binary_table(rows, width))
    for query in random_rows(1000 + width, 3, width):
        kwargs = dict(column="vector", q=query.tolist(), k=10)
        expect_all = sorted(zip(oracle(rows, query).tolist(), range(200)))
        a = theirs.to_table(nearest=dict(kwargs, k=200), with_row_id=True)
        b = ours.to_table(nearest=dict(kwargs, k=200), with_row_id=True)
        assert sorted(zip(a["_distance"].to_pylist(), a["id"].to_pylist())) == expect_all
        assert sorted(zip(b["_distance"].to_pylist(), b["id"].to_pylist())) == expect_all
        # The distances of the top k do not depend on how ties are broken, so they match exactly.
        top = [d for d, _ in expect_all[:10]]
        assert theirs.to_table(nearest=kwargs)["_distance"].to_pylist() == pytest.approx(top, abs=0.0)
        assert ours.to_table(nearest=kwargs)["_distance"].to_pylist() == pytest.approx(top, abs=0.0)
        assert ours.to_table(nearest=kwargs)["_distance"].type == pa.float32()


def test_explicit_hamming_is_the_default(lance, tmp_path):
    rows = random_rows(7, 50, 8)
    theirs, ours = write_both(lance, tmp_path, binary_table(rows, 8))
    query = random_rows(8, 1, 8)[0].tolist()
    for ds in (theirs, ours):
        default = ds.to_table(nearest={"column": "vector", "q": query, "k": 5})
        explicit = ds.to_table(nearest={"column": "vector", "q": query, "k": 5, "metric": "hamming"})
        assert explicit.equals(default)
        assert "metric=hamming" in ds.scanner(nearest={"column": "vector", "q": query, "k": 5}).analyze_plan()


@pytest.mark.parametrize("metric", ["l2", "cosine", "dot"])
def test_other_metrics_are_refused(lance, tmp_path, metric):
    theirs, ours = write_both(lance, tmp_path, binary_table(random_rows(3, 10, 4), 4))
    for ds in (theirs, ours):
        with pytest.raises(ValueError, match=f"Distance type {metric} does not support .*UInt8 vectors"):
            ds.to_table(nearest={"column": "vector", "q": [1, 2, 3, 4], "k": 1, "metric": metric})


def test_size_mismatch_is_refused(lance, tmp_path):
    theirs, ours = write_both(lance, tmp_path, binary_table(random_rows(4, 5, 4), 4))
    for ds in (theirs, ours):
        with pytest.raises(ValueError, match="Query vector size 3 does not match index column size 4"):
            ds.to_table(nearest={"column": "vector", "q": [1, 2, 3], "k": 1})


def test_null_vectors_are_dropped(lance, tmp_path):
    rows = random_rows(5, 40, 8)
    theirs, ours = write_both(lance, tmp_path, binary_table(rows, 8, nulls={1, 3, 30}))
    query = random_rows(6, 1, 8)[0]
    keep = [i for i in range(40) if i not in (1, 3, 30)]
    expect = sorted(zip(oracle(rows[keep], query).tolist(), keep))
    for ds in (theirs, ours):
        got = ds.to_table(nearest={"column": "vector", "q": query.tolist(), "k": 40}, with_row_id=True)
        assert sorted(zip(got["_distance"].to_pylist(), got["id"].to_pylist())) == expect


@pytest.mark.parametrize("prefilter", [True, False])
def test_filters_match_pylance(lance, tmp_path, prefilter):
    rows = random_rows(9, 120, 8)
    theirs, ours = write_both(lance, tmp_path, binary_table(rows, 8))
    query = random_rows(10, 1, 8)[0]
    # prefilter: the k nearest among the matching rows; post-filter: the k nearest, then the filter, so
    # asking for every row keeps it exact.
    args = dict(filter="id % 3 = 0", prefilter=prefilter)
    keep = [i for i in range(120) if i % 3 == 0]
    k = 6 if prefilter else 120
    distances = {}
    for name, ds in (("pylance", theirs), ("nanolance", ours)):
        got = ds.to_table(nearest={"column": "vector", "q": query.tolist(), "k": k}, with_row_id=True, **args)
        ids = got["id"].to_pylist()
        assert set(ids) <= set(keep), name
        # each returned row carries its true Hamming distance
        assert got["_distance"].to_pylist() == oracle(rows[ids], query).astype(np.float32).tolist(), name
        distances[name] = got["_distance"].to_pylist()
    assert distances["nanolance"] == distances["pylance"]
    if prefilter:
        assert len(distances["nanolance"]) == k


def test_row_ids_and_bounds(lance, tmp_path):
    rows = random_rows(11, 60, 16)
    theirs, ours = write_both(lance, tmp_path, binary_table(rows, 16))
    query = random_rows(12, 1, 16)[0]
    for ds in (theirs, ours):
        got = ds.to_table(nearest={"column": "vector", "q": query.tolist(), "k": 60}, with_row_id=True)
        assert got["_rowid"].to_pylist() == got["id"].to_pylist()
        bounded = ds.to_table(nearest={"column": "vector", "q": query.tolist(), "k": 60,
                                       "distance_range": (10, 40)})
        assert all(10 <= d < 40 for d in bounded["_distance"].to_pylist())


def test_ivf_index_on_binary_needs_hamming(tmp_path):
    # An IVF_FLAT index over uint8 vectors is built with metric="hamming" (test_binary_ivf.py); any other metric is refused.
    ours = nl.write_dataset(binary_table(random_rows(13, 300, 8), 8), str(tmp_path / "nanolance"))
    with pytest.raises(ValueError, match="does not support UInt8 vectors, use hamming"):
        ours.create_index("vector", "IVF_FLAT", num_partitions=2)


@pytest.mark.parametrize("width", [8, 16])
def test_hamming_clustering_matches_pylance(lance, tmp_path, width):
    rows = random_rows(14, 300, width)
    rows[10] = rows[11]
    rows[20] = rows[10]
    rows[21] = rows[10]
    rows[21, 0] ^= 1
    theirs, ours = write_both(lance, tmp_path, binary_table(rows, width))
    for threshold in (0, 8, 16):
        expect = importlib.import_module("lance.vector").hamming_clustering_for_sample(
            theirs, "vector", None, threshold).read_all()
        got = nl_clustering(ours, "vector", None, threshold).read_all()
        assert got.to_pylist() == expect.to_pylist()


def test_hamming_clustering_example(tmp_path):
    hash_a = [0] * 8
    hash_b = [255, 0, 0, 0, 0, 0, 0, 0]  # 8 bits from hash_a
    hash_c = list(range(1, 9))
    table = binary_table([hash_a, hash_a, hash_a, hash_b, hash_b, hash_c], 8)
    ours = nl.write_dataset(table, str(tmp_path / "hashes"))
    clusters = lambda t: {r: sorted(d) for r, d in zip(t["representative"].to_pylist(), t["duplicates"].to_pylist())}
    assert clusters(nl_clustering(ours, "vector", None, 0).read_all()) == {0: [1, 2], 3: [4]}
    assert clusters(nl_clustering(ours, "vector", None, 8).read_all()) == {0: [1, 2, 3, 4]}
