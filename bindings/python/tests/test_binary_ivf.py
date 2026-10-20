"""Hamming IVF_FLAT indexes (on a fixed-size list of uint8) built by pylance and read by nanolance: the same ids and
distances on the same index, the same stats, partition sizes and clustering, across two segments. nanolance does
not build these indexes yet (the build is refused), so every index here is pylance's."""

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


def search(ds, nearest, filter_sql=None):
    table = ds.to_table(columns=["id"], nearest=nearest, filter=filter_sql)
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
    assert "metric=hamming" in ours and "metric=hamming" in theirs.lower()
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
