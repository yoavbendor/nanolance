"""Hamming IVF_FLAT indexes (on a fixed-size list of uint8) read and built by nanolance, against pylance: the same
ids and distances on the same index, the same stats, partition sizes and clustering, across two segments; and
nanolance's indexes searched by pylance, its IvfModel files loaded by pylance and back. Indexes are built by
pylance (the H1 tests) or by nanolance (the H2 tests), as each test says."""

from __future__ import annotations

import importlib
import json

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance import vector as nl_vector
from tests.support import require_pylance

PV = None  # pylance's lance.vector, imported with the fixture below
DIM = 16
ROWS = 1000


@pytest.fixture(scope="module")
def lance():
    global PV
    module = require_pylance()
    PV = importlib.import_module("lance.vector")
    return module


def binary_table(rows, start=0):
    values = pa.array(np.asarray(rows, dtype=np.uint8).ravel(), type=pa.uint8())
    vectors = pa.FixedSizeListArray.from_arrays(values, DIM)
    ids = pa.array(range(start, start + len(rows)), pa.int32())
    return pa.table({"vector": vectors, "id": ids})


def random_rows(seed, n):
    rng = np.random.default_rng(seed)
    rows = rng.integers(0, 256, (n, DIM), dtype=np.uint8)
    if n > 13:  # duplicates and near-duplicates for the clustering checks
        rows[10] = rows[11]
        rows[12] = rows[11]
        rows[13] = rows[11] ^ np.uint8(1)
    return rows


def pylance_index(lance, tmp_path, rows, num_partitions=4):
    path = str(tmp_path / "binary.lance")
    ds = lance.write_dataset(binary_table(rows), path)
    ds.create_index("vector", index_type="IVF_FLAT", num_partitions=num_partitions, metric="hamming")
    return path


def search(ds, nearest, filter_sql=None, prefilter=False):
    table = ds.to_table(columns=["id"], nearest=nearest, filter=filter_sql, prefilter=prefilter)
    return table["id"].to_pylist(), table["_distance"].to_pylist()


@pytest.mark.parametrize("k", [1, 10, 50])
@pytest.mark.parametrize("nprobes", [1, 4])
@pytest.mark.parametrize("filter_sql", [None, "id < 500"])
def test_nanolance_searches_pylance_hamming_index_like_pylance(lance, tmp_path, k, nprobes, filter_sql):
    path = pylance_index(lance, tmp_path, random_rows(1, ROWS))
    theirs = lance.dataset(path)
    theirs.delete("id < 100")  # a deletion after the build: both engines must hide the row
    ours = nl.dataset(path)
    query = random_rows(2, 1)[0]
    nearest = {"column": "vector", "q": query, "k": k, "nprobes": nprobes}
    assert search(ours, nearest, filter_sql) == search(theirs, nearest, filter_sql)


def test_default_metric_is_the_index_metric(lance, tmp_path):
    path = pylance_index(lance, tmp_path, random_rows(3, ROWS))
    query = random_rows(4, 1)[0]
    nearest = {"column": "vector", "q": query, "k": 5}
    ours = nl.dataset(path)
    theirs = lance.dataset(path)
    assert search(ours, nearest) == search(theirs, nearest)
    explicit = {**nearest, "metric": "hamming"}
    assert search(ours, explicit) == search(ours, nearest)


def test_refine_factor_rescores_like_pylance(lance, tmp_path):
    path = pylance_index(lance, tmp_path, random_rows(5, ROWS))
    nearest = {"column": "vector", "q": random_rows(6, 1)[0], "k": 10, "nprobes": 2, "refine_factor": 3}
    assert search(nl.dataset(path), nearest) == search(lance.dataset(path), nearest)


def test_stats_match_pylance(lance, tmp_path):
    path = pylance_index(lance, tmp_path, random_rows(7, ROWS))
    expect = lance.dataset(path).stats.index_stats("vector_idx")
    got = nl.dataset(path).stats.index_stats("vector_idx")
    for stats in (expect, got):
        for key in ("uri", "uuid", "loss", "index_file_version"):
            stats["indices"][0].pop(key, None)
            stats.pop(key, None)
    assert json.dumps(got, sort_keys=True) == json.dumps(expect, sort_keys=True)
    assert got["indices"][0]["metric_type"] == "hamming"
    assert got["indices"][0]["sub_index"] == {"dim": DIM, "index_type": "FLAT", "metric_type": "hamming"}


def test_plan_names_the_hamming_index(lance, tmp_path):
    path = pylance_index(lance, tmp_path, random_rows(8, ROWS))
    nearest = {"column": "vector", "q": random_rows(9, 1)[0], "k": 5, "nprobes": 2}
    ours = nl.dataset(path).scanner(nearest=nearest, columns=["id"]).explain_plan()
    theirs = lance.dataset(path).scanner(nearest=nearest, columns=["id"]).explain_plan()
    assert "ANNSubIndex: name=vector_idx, k=5" in ours
    assert "metric=Hamming" in ours and "metric=hamming" in theirs.lower()  # the index line is spelt Hamming
    assert "nprobes=2" in ours


def two_segments(lance, tmp_path):
    """Fragment 0 indexed by pylance, fragment 1 appended and indexed as a delta segment."""
    rows = random_rows(10, 300)
    path = str(tmp_path / "segments.lance")
    ds = lance.write_dataset(binary_table(rows), path)
    ds.create_index("vector", index_type="IVF_FLAT", num_partitions=4, metric="hamming")
    lance.write_dataset(binary_table(rows, start=1000), path, mode="append")
    ds = lance.dataset(path)
    ds.optimize.optimize_indices(num_indices_to_merge=0)
    return path


def test_partition_info_across_two_segments(lance, tmp_path):
    path = two_segments(lance, tmp_path)
    theirs, ours = lance.dataset(path), nl.dataset(path)
    assert len(ours.list_indices()) == 2
    segments = [s["uuid"] for s in ours.list_indices()]
    for selected in (None, [segments[0]], [segments[1]]):
        expect = PV.get_ivf_partition_info(theirs, "vector_idx", index_segments=selected)
        got = nl_vector.get_ivf_partition_info(ours, "vector_idx", index_segments=selected)
        assert got == expect
    assert sum(p["size"] for p in nl_vector.get_ivf_partition_info(ours, "vector_idx")) == 600


def test_partition_info_rejects_bad_segments(lance, tmp_path):
    path = two_segments(lance, tmp_path)
    ours = nl.dataset(path)
    with pytest.raises(ValueError, match="invalid index segment uuid"):
        nl_vector.get_ivf_partition_info(ours, "vector_idx", index_segments=["not-a-uuid"])
    with pytest.raises(TypeError, match="str or uuid.UUID"):
        nl_vector.get_ivf_partition_info(ours, "vector_idx", index_segments=[123])
    with pytest.raises(TypeError, match="not a single"):
        nl_vector.get_ivf_partition_info(ours, "vector_idx", index_segments="x")


@pytest.mark.parametrize("threshold", [0, 3, 20])
def test_hamming_clustering_for_ivf_partition_matches_pylance(lance, tmp_path, threshold):
    path = two_segments(lance, tmp_path)
    theirs, ours = lance.dataset(path), nl.dataset(path)
    segments = [s["uuid"] for s in ours.list_indices()]
    for selected in (None, [segments[0]]):
        for partition in range(4):
            expect = PV.hamming_clustering_for_ivf_partition(
                theirs, "vector_idx", partition, threshold, index_segments=selected).read_all()
            got = nl_vector.hamming_clustering_for_ivf_partition(
                ours, "vector_idx", partition, threshold, index_segments=selected).read_all()
            assert got.to_pydict() == expect.to_pydict()


def test_partition_out_of_range_is_refused(lance, tmp_path):
    path = two_segments(lance, tmp_path)
    with pytest.raises(ValueError, match="Partition ID 9 is out of range"):
        nl_vector.hamming_clustering_for_ivf_partition(nl.dataset(path), "vector_idx", 9, 0)


# ── H2: nanolance builds Hamming IVF_FLAT, pylance searches it ──────────────────────────────


def brute_force(rows, query, k):
    """The k smallest Hamming distances from `query` to `rows` (sorted), and every row's distance."""
    bits = np.unpackbits(rows ^ query, axis=1).sum(axis=1)
    return np.sort(bits)[:k].astype(float).tolist(), bits


def nanolance_index(tmp_path, rows, num_partitions=4, name="binary.lance", seed=7):
    path = str(tmp_path / name)
    nl.write_dataset(binary_table(rows), path).create_index(
        "vector", index_type="IVF_FLAT", num_partitions=num_partitions, metric="hamming", seed=seed)
    return path


@pytest.mark.parametrize("k", [1, 10, 50])
@pytest.mark.parametrize("filter_sql", [None, "id < 500"])
def test_pylance_searches_nanolance_hamming_index_exactly(lance, tmp_path, k, filter_sql):
    rows = random_rows(20, ROWS)
    path = nanolance_index(tmp_path, rows)
    query = random_rows(21, 1)[0]
    nearest = {"column": "vector", "q": query, "k": k, "nprobes": 4}
    ids, distances = search(lance.dataset(path), nearest, filter_sql, prefilter=bool(filter_sql))
    limit = 500 if filter_sql else ROWS
    want, bits = brute_force(rows[:limit], query, k)
    assert distances == want
    assert all(i < limit for i in ids)
    assert distances == [float(bits[i]) for i in ids]  # each returned row's distance is its own


def test_nanolance_hamming_index_recall_at_least_pylances(lance, tmp_path):
    # k-means is seeded on nanolance's side and random on pylance's, so the recall of several builds on each side
    # is compared: the mean of nanolance's must not fall below pylance's.
    rows = random_rows(22, ROWS)
    queries = random_rows(23, 40)
    ours = [nanolance_index(tmp_path, rows, name=f"ours{seed}.lance", seed=seed) for seed in range(3)]
    theirs = []
    for n in range(3):
        path = str(tmp_path / f"theirs{n}.lance")
        lance.write_dataset(binary_table(rows), path).create_index(
            "vector", index_type="IVF_FLAT", num_partitions=4, metric="hamming")
        theirs.append(path)

    def recall(paths, nprobes):
        found = []
        for path in paths:
            ds = lance.dataset(path)
            hits = 0
            for query in queries:
                got, _ = search(ds, {"column": "vector", "q": query, "k": 10, "nprobes": nprobes})
                bits = np.unpackbits(rows ^ query, axis=1).sum(axis=1)
                tenth = np.sort(bits)[9]
                hits += sum(1 for i in got if bits[i] <= tenth)  # ties at the tenth distance count as found
            found.append(hits / (10 * len(queries)))
        return float(np.mean(found))

    assert recall(ours, 1) >= recall(theirs, 1) - 0.05
    assert recall(ours, 4) == 1.0  # every partition probed: exact


def test_nanolance_hamming_index_stats_and_plan_read_by_pylance(lance, tmp_path):
    path = nanolance_index(tmp_path, random_rows(24, ROWS))
    stats = lance.dataset(path).stats.index_stats("vector_idx")
    assert stats["index_type"] == "IVF_FLAT"
    assert stats["indices"][0]["metric_type"] == "hamming"
    assert stats["indices"][0]["sub_index"] == {"dim": DIM, "index_type": "FLAT", "metric_type": "hamming"}
    plan = lance.dataset(path).scanner(nearest={"column": "vector", "q": random_rows(25, 1)[0], "k": 5}).explain_plan()
    assert "metric=Hamming" in plan


def test_nanolance_default_partitions_are_rows_over_4096(tmp_path):
    # create_index without num_partitions: Lance's target partition size for IVF_FLAT (4096 rows).
    path = str(tmp_path / "default.lance")
    ds = nl.write_dataset(binary_table(random_rows(26, 20000)), path)
    ds.create_index("vector", index_type="IVF_FLAT", metric="hamming", seed=1)
    assert len(nl_vector.get_ivf_partition_info(nl.dataset(path), "vector_idx")) == 20000 // 4096


def test_nanolance_refuses_hamming_on_float_and_l2_on_binary(tmp_path):
    floats = nl.write_dataset(pa.table({"v": pa.array([[0.5, 1.0]] * 300, pa.list_(pa.float32(), 2))}),
                              str(tmp_path / "f.lance"))
    with pytest.raises(ValueError, match="hamming does not support Float32"):
        floats.create_index("v", index_type="IVF_FLAT", num_partitions=2, metric="hamming")
    binary = nl.write_dataset(binary_table(random_rows(27, 300)), str(tmp_path / "b.lance"))
    with pytest.raises(ValueError, match="does not support UInt8 vectors, use hamming"):
        binary.create_index("vector", index_type="IVF_FLAT", num_partitions=2, metric="l2")
    with pytest.raises(ValueError, match="must be IVF_FLAT"):
        binary.create_index("vector", index_type="IVF_PQ", num_partitions=2, num_sub_vectors=4, metric="hamming")


def test_ivf_model_files_cross_loaded(lance, tmp_path):
    from lance.indices import IndicesBuilder as PyBuilder, IvfModel as PyIvfModel
    from nanolance.lance.indices import IndicesBuilder, IvfModel

    rows = random_rows(28, 400)
    ours = IndicesBuilder(nl.write_dataset(binary_table(rows), str(tmp_path / "ours.lance")), "vector").train_ivf(
        num_partitions=6, distance_type="hamming", sample_rate=8, max_iters=10)
    assert ours.distance_type == "hamming"
    ours.save(str(tmp_path / "ours.idx"))
    loaded = PyIvfModel.load(str(tmp_path / "ours.idx"))
    assert loaded.distance_type == "hamming"
    assert loaded.centroids.to_pylist() == ours.centroids.to_pylist()

    theirs = PyBuilder(lance.write_dataset(binary_table(rows), str(tmp_path / "theirs.lance")), "vector").train_ivf(
        num_partitions=6, distance_type="hamming", sample_rate=8, max_iters=10)
    theirs.save(str(tmp_path / "theirs.idx"))
    back = IvfModel.load(str(tmp_path / "theirs.idx"))
    assert back.distance_type == "hamming" and len(back.centroids) == 6
    assert back.centroids.type == theirs.centroids.type


def test_delta_segment_after_append(lance, tmp_path):
    first = random_rows(29, 300)
    second = random_rows(30, 200)
    path = nanolance_index(tmp_path, first, name="delta.lance")
    nl.write_dataset(binary_table(second, start=300), path, mode="append")
    nl.dataset(path).optimize.optimize_indices(num_indices_to_merge=0)
    ours = nl.dataset(path)
    assert len(ours.describe_indices()[0].segments) == 2
    infos = nl_vector.get_ivf_partition_info(ours, "vector_idx")
    assert sum(info["size"] for info in infos) == 500

    rows = np.vstack([first, second])
    query = random_rows(31, 1)[0]
    ids, distances = search(lance.dataset(path), {"column": "vector", "q": query, "k": 20, "nprobes": 4})
    want, bits = brute_force(rows, query, 20)
    assert distances == want
    assert distances == [float(bits[i]) for i in ids]


def test_nanolance_hamming_centroids_are_uint8(tmp_path):
    from nanolance.lance.indices import IndicesBuilder

    path = str(tmp_path / "u8.lance")
    model = IndicesBuilder(nl.write_dataset(binary_table(random_rows(32, 300)), path), "vector").train_ivf(
        num_partitions=3, distance_type="hamming", sample_rate=8, max_iters=5)
    assert model.centroids.type == pa.list_(pa.uint8(), DIM)
    assert len(model.centroids) == 3
