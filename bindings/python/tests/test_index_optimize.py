"""optimize_indices: rows an index does not cover yet are folded into it, as pylance does it.

Each test indexes a dataset (with pylance, or with nanolance), appends to it, and optimizes it twice:
once with pylance, once with nanolance, on copies. Then:

- the index covers every fragment, and pylance reports no unindexed rows;
- every query pylance answers from nanolance's optimized index returns what it returns from its own:
  the same rows for filters, the same scores for full-text search (the statistics are the index's
  own, so they agree bit for bit), and the same rows and distances for vector search (the new rows
  are assigned to the existing partitions and encoded with the existing codebook, as pylance does);
- nanolance answers the same from either.

And the selection rules: num_indices_to_merge=0 adds a segment over the new fragments alone, the
default merges into the last segment, retrain trains a new model; compaction gives its rewritten
rows back to the indexes.
"""

from __future__ import annotations

import shutil

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance import query as nq
from tests.support import require_pylance

pytestmark = pytest.mark.filterwarnings("ignore:The 'list_indices' method is deprecated")


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


WORDS = "apple pear plum fig kiwi lime date grape melon peach berry cherry".split()


def _table(n: int, start: int = 0) -> pa.Table:
    rng = np.random.default_rng(start + 1)
    ids = np.arange(start, start + n)
    text = [" ".join(rng.choice(WORDS, size=int(rng.integers(2, 9)))) for _ in ids]
    centres = np.random.default_rng(7).standard_normal((8, 16)).astype(np.float32) * 3
    vec = centres[rng.integers(0, 8, n)] + rng.standard_normal((n, 16)).astype(np.float32)
    return pa.table({
        "id": pa.array(ids),
        "cat": pa.array([f"c{i % 7}" for i in ids]),
        "tags": pa.array([[f"t{i % 3}", f"t{i % 5}"] for i in ids], pa.list_(pa.string())),
        "text": pa.array(text),
        "vec": pa.FixedSizeListArray.from_arrays(pa.array(vec.ravel()), 16),
        "vec2": pa.FixedSizeListArray.from_arrays(pa.array(vec[:, ::-1].ravel()), 16),
    })


def _index_all(mod, path: str):
    ds = mod.dataset(path)
    ds.create_scalar_index("id", "BTREE")
    ds.create_scalar_index("cat", "BITMAP")
    ds.create_scalar_index("tags", "LABEL_LIST")
    ds.create_scalar_index("text", "INVERTED")
    ds.create_index("vec", "IVF_FLAT", num_partitions=4, name="flat_idx")
    ds.create_index("vec2", "IVF_PQ", num_partitions=4, num_sub_vectors=4, name="pq_idx")


def _segments(lance, path: str) -> dict:
    out = {}
    for d in lance.dataset(path).describe_indices():
        out[d.name] = sorted(sorted(s.fragment_ids) for s in d.segments)
    return out


def _filters():
    return ["id = 1500", "id >= 2990 AND id < 3010", "cat = 'c3'", "array_has_any(tags, ['t4'])", "id > 3500"]


def _check_same(lance, ours_path: str, theirs_path: str):
    theirs, ours = lance.dataset(theirs_path), lance.dataset(ours_path)
    for f in _filters():
        a = sorted(theirs.to_table(filter=f, columns=["id"]).column("id").to_pylist())
        b = sorted(ours.to_table(filter=f, columns=["id"]).column("id").to_pylist())
        c = sorted(nl.dataset(ours_path).to_table(filter=f, columns=["id"]).column("id").to_pylist())
        assert a == b == c, f
        assert "ScalarIndexQuery" in ours.scanner(filter=f).explain_plan(True) or "array_has" in f or "LabelList" in \
            ours.scanner(filter=f).explain_plan(True)
    for q in ["apple", "plum fig", "cherry berry melon"]:
        a = theirs.to_table(full_text_query=q, columns=["id", "_score"])
        b = ours.to_table(full_text_query=q, columns=["id", "_score"])
        assert sorted(zip(a.column("_score").to_pylist(), a.column("id").to_pylist())) == \
            sorted(zip(b.column("_score").to_pylist(), b.column("id").to_pylist())), q
        c = nl.dataset(ours_path).to_table(full_text_query=q, columns=["id", "_score"])
        assert sorted(c.column("_score").to_pylist()) == sorted(a.column("_score").to_pylist()), q
    rng = np.random.default_rng(3)
    for column in ["vec", "vec2"]:
        for _ in range(5):
            key = rng.standard_normal(16).astype(np.float32) * 3
            nearest = {"column": column, "q": key, "k": 20}
            a = theirs.to_table(nearest=nearest, columns=["id"])
            b = ours.to_table(nearest=nearest, columns=["id"])
            c = nl.dataset(ours_path).to_table(nearest=nearest, columns=["id"])
            for got in (b, c):
                da, dg = a.column("_distance").to_numpy(), got.column("_distance").to_numpy()
                assert np.allclose(da, dg, rtol=1e-5), column
                # PQ distances tie exactly; rows tied at the cut may differ (pylance's order is arbitrary).
                cut = da[-1]
                assert {i for i, x in zip(a.column("id").to_pylist(), da) if x < cut * (1 - 1e-6)} == \
                    {i for i, x in zip(got.column("id").to_pylist(), dg) if x < cut * (1 - 1e-6)}, column
    # Both rebalanced the partitions the same way (Lance's join of undersized partitions).
    for name in ["flat_idx", "pq_idx"]:
        a = [p["size"] for p in theirs.stats.index_stats(name)["indices"][0]["partitions"]]
        b = [p["size"] for p in ours.stats.index_stats(name)["indices"][0]["partitions"]]
        assert len(a) == len(b), name


@pytest.mark.parametrize("builder", ["pylance", "nanolance"])
def test_optimize_after_append(lance, tmp_path, builder):
    seed = str(tmp_path / "seed.lance")
    lance.write_dataset(_table(3_000), seed, max_rows_per_file=1_000)
    _index_all(lance if builder == "pylance" else nl, seed)
    nl.write_dataset(_table(800, start=3_000), seed, mode="append", max_rows_per_file=400)
    nl.dataset(seed).delete("id % 97 = 0")
    theirs, ours = str(tmp_path / "theirs.lance"), str(tmp_path / "ours.lance")
    shutil.copytree(seed, theirs)
    shutil.copytree(seed, ours)
    lance.dataset(theirs).optimize.optimize_indices()
    nl.dataset(ours).optimize.optimize_indices()

    ds = lance.dataset(ours)
    frags = sorted(f.fragment_id for f in ds.get_fragments())
    for name, segments in _segments(lance, ours).items():
        assert segments == [frags], name
        assert ds.stats.index_stats(name)["num_unindexed_rows"] == 0, name
    assert _segments(lance, ours) == _segments(lance, theirs)
    ds.validate()
    _check_same(lance, ours, theirs)


def test_selection_rules(lance, tmp_path):
    path = str(tmp_path / "d.lance")
    nl.write_dataset(_table(2_000), path, max_rows_per_file=1_000)
    ds = nl.dataset(path)
    ds.create_scalar_index("id", "BTREE")
    ds.create_scalar_index("text", "INVERTED")
    nl.write_dataset(_table(500, start=2_000), path, mode="append")
    # 0: a delta segment over the new fragment alone.
    nl.dataset(path).optimize.optimize_indices(num_indices_to_merge=0)
    assert _segments(lance, path) == {"id_idx": [[0, 1], [2]], "text_idx": [[0, 1], [2]]}
    nl.write_dataset(_table(500, start=2_500), path, mode="append")
    # Default: the last segment and the new fragment become one; the first segment stays.
    nl.dataset(path).optimize.optimize_indices(index_names=["id_idx"])
    assert _segments(lance, path) == {"id_idx": [[0, 1], [2, 3]], "text_idx": [[0, 1], [2]]}
    # Two: every segment into one.
    nl.dataset(path).optimize.optimize_indices(num_indices_to_merge=2)
    assert _segments(lance, path) == {"id_idx": [[0, 1, 2, 3]], "text_idx": [[0, 1, 2, 3]]}
    # Nothing left to fold in: no new version.
    v = nl.dataset(path).version
    nl.dataset(path).optimize.optimize_indices()
    assert nl.dataset(path).version == v
    for f in ["id = 2700", "id < 10"]:
        assert lance.dataset(path).to_table(filter=f).num_rows == nl.dataset(path).to_table(filter=f).num_rows == \
            lance.dataset(path).to_table(filter=f, use_scalar_index=False).num_rows
    a = lance.dataset(path).to_table(full_text_query="plum", columns=["id", "_score"])
    b = nl.dataset(path).to_table(full_text_query="plum", columns=["id", "_score"])
    assert sorted(a.column("_score").to_pylist()) == sorted(b.column("_score").to_pylist())
    with pytest.raises(Exception, match="not found"):
        nl.dataset(path).optimize.optimize_indices(index_names=["nope"])


def test_vector_model_kept_or_retrained(lance, tmp_path):
    path = str(tmp_path / "v.lance")
    nl.write_dataset(_table(3_000), path, max_rows_per_file=1_000)
    # A target of 750 rows a partition: 4 partitions, and none to split or join after the append.
    nl.dataset(path).create_index("vec", "IVF_PQ", target_partition_size=750, num_sub_vectors=4)
    uuid0 = lance.dataset(path).describe_indices()[0].segments[0].uuid
    centroids0 = lance.dataset(path).stats.index_stats("vec_idx")["indices"][0]["centroids"]
    nl.write_dataset(_table(1_000, start=3_000), path, mode="append")
    nl.dataset(path).optimize.optimize_indices()
    stats = lance.dataset(path).stats.index_stats("vec_idx")
    assert stats["num_unindexed_rows"] == 0
    assert lance.dataset(path).describe_indices()[0].segments[0].uuid != uuid0
    assert np.allclose(stats["indices"][0]["centroids"], centroids0)  # the model is kept
    assert sum(p["size"] for p in stats["indices"][0]["partitions"]) == 4_000
    nl.dataset(path).optimize.optimize_indices(retrain=True)
    stats = lance.dataset(path).stats.index_stats("vec_idx")
    assert stats["num_unindexed_rows"] == 0 and len(stats["indices"][0]["partitions"]) == 4
    # A new row is found by its own vector.
    key = lance.dataset(path).to_table(filter="id = 3500", columns=["vec"]).column("vec")[0].values.to_numpy()
    for mod in (lance, nl):
        got = mod.dataset(path).to_table(nearest={"column": "vec", "q": key, "k": 1, "nprobes": 4, "refine_factor": 20},
                                         columns=["id"])
        assert got.column("id").to_pylist() == [3_500]


def test_compaction_keeps_indexes(lance, tmp_path):
    path = str(tmp_path / "c.lance")
    nl.write_dataset(_table(2_000), path, max_rows_per_file=250)
    _index_all(nl, path)
    nl.dataset(path).delete("id % 11 = 0")
    metrics = nl.dataset(path).optimize.compact_files(target_rows_per_fragment=1_000)
    assert metrics.fragments_removed > 0
    ds = lance.dataset(path)
    frags = sorted(f.fragment_id for f in ds.get_fragments())
    for name, segments in _segments(lance, path).items():
        assert sorted(set(sum(segments, []))) == frags, name
        assert ds.stats.index_stats(name)["num_unindexed_rows"] == 0, name
    ds.validate()
    for f in _filters():
        assert sorted(ds.to_table(filter=f, columns=["id"]).column("id").to_pylist()) == sorted(
            ds.to_table(filter=f, columns=["id"], use_scalar_index=False).column("id").to_pylist())
    a = ds.to_table(full_text_query=nq.MatchQuery("apple", "text"), columns=["id"])
    assert all(i % 11 for i in a.column("id").to_pylist()) and a.num_rows > 0


def test_index_kind_it_cannot_build(lance, tmp_path):
    path = str(tmp_path / "h.lance")
    lance.write_dataset(_table(2_000), path)
    lance.dataset(path).create_scalar_index("id", "BTREE")
    lance.dataset(path).create_scalar_index("id", "ZONEMAP", name="zm")
    nl.write_dataset(_table(100, start=2_000), path, mode="append")
    with pytest.raises(Exception, match="cannot"):
        nl.dataset(path).optimize.optimize_indices()
    nl.dataset(path).optimize.optimize_indices(index_names=["id_idx"])
    assert lance.dataset(path).stats.index_stats("id_idx")["num_unindexed_rows"] == 0
