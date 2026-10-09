"""Distributed index builds, as pylance 12 does them: segments built apart, merged, committed.

``create_index_uncommitted`` builds one segment over chosen fragments without committing it;
``merge_existing_index_segments`` folds a group of them into one; ``commit_existing_index_segments``
publishes them as one logical index. ``IndicesBuilder`` trains the shared IVF / PQ model the vector
segments are built with. Each test checks nanolance against pylance both ways:

- segments nanolance builds (and merges, and commits) are an index pylance answers from as it
  answers from the index it builds itself in one go -- the same rows for filters, the same scores
  for full-text search, the same rows and distances for vector search with a shared model;
- segments pylance builds are merged and committed by nanolance, and the other way round;
- models nanolance trains load in pylance (IvfModel / PqModel files) and pylance's in nanolance.
"""

from __future__ import annotations

import shutil
import uuid

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance.indices import IndicesBuilder, IvfModel, PqModel
from tests.support import require_pylance

pytestmark = pytest.mark.filterwarnings("ignore:The 'list_indices' method is deprecated")

WORDS = "apple pear plum fig kiwi lime date grape melon peach berry cherry".split()
DIM = 16
ROWS_PER_FRAGMENT = 160
FRAGMENTS = 4


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _table() -> pa.Table:
    n = ROWS_PER_FRAGMENT * FRAGMENTS
    rng = np.random.default_rng(3)
    ids = np.arange(n)
    centres = np.random.default_rng(7).standard_normal((8, DIM)).astype(np.float32) * 3
    vec = centres[rng.integers(0, 8, n)] + rng.standard_normal((n, DIM)).astype(np.float32)
    return pa.table({
        "id": pa.array(ids),
        "cat": pa.array([f"c{i % 7}" for i in ids]),
        "tags": pa.array([[f"t{i % 3}", f"t{i % 5}"] for i in ids], pa.list_(pa.string())),
        "text": pa.array([" ".join(rng.choice(WORDS, size=int(rng.integers(2, 9)))) for _ in ids]),
        "vec": pa.FixedSizeListArray.from_arrays(pa.array(vec.reshape(-1)), DIM),
    })


@pytest.fixture
def make(tmp_path):
    """A fresh copy of the test dataset, written by nanolance, at tmp_path/<name>."""
    table = _table()

    def write(name: str) -> str:
        uri = str(tmp_path / name)
        nl.write_dataset(table, uri, max_rows_per_file=ROWS_PER_FRAGMENT)
        return uri

    return write


SCALAR = {
    "BTREE": ("id", ["id < 100", "id >= 500 AND id < 520", "id IN (3, 333, 600)"]),
    "BITMAP": ("cat", ["cat = 'c3'", "cat IN ('c1', 'c6')"]),
    "LABEL_LIST": ("tags", ["array_has_any(tags, ['t2'])", "array_has_all(tags, ['t0', 't4'])"]),
}


def _groups(ds) -> list:
    ids = [f.fragment_id for f in ds.get_fragments()]
    return [ids[:2], ids[2:]]


def _rows(ds, flt) -> list:
    return sorted(ds.to_table(filter=flt, columns=["id"])["id"].to_pylist())


def _fts(ds, query) -> dict:
    t = ds.to_table(full_text_query=query, columns=["id", "_score"])
    return dict(zip(t["id"].to_pylist(), np.round(t["_score"].to_numpy(), 4).tolist()))


def _pylance_index(lance, index):
    """A nanolance ``Index`` as pylance's own dataclass."""
    from lance.dataset import Index, IndexFile

    return Index(uuid=index.uuid, name=index.name, fields=list(index.fields),
                 dataset_version=index.dataset_version, fragment_ids=set(index.fragment_ids),
                 index_version=index.index_version, created_at=index.created_at,
                 files=[IndexFile(f.path, f.size_bytes) for f in index.files], index_details=index.index_details)


@pytest.mark.parametrize("kind", sorted(SCALAR))
def test_scalar_segments_merged_and_committed(lance, make, kind):
    column, filters = SCALAR[kind]
    # nanolance: a segment per fragment group, merged into one, committed.
    ours = nl.dataset(make("ours"))
    segments = [ours.create_index_uncommitted(column, kind, name="idx", fragment_ids=g) for g in _groups(ours)]
    assert len({s.uuid for s in segments}) == 2
    assert [set(s.fragment_ids) for s in segments] == [set(g) for g in _groups(ours)]
    assert ours.list_indices() == []  # nothing committed yet
    merged = ours.merge_existing_index_segments(segments)
    assert merged.uuid not in {s.uuid for s in segments}
    assert set(merged.fragment_ids) == set(range(FRAGMENTS))
    assert merged.name == "idx" and merged.index_details is not None
    ours.commit_existing_index_segments("idx", column, [merged])
    # pylance: the same index built in one go.
    whole = lance.dataset(make("whole"))
    whole.create_scalar_index(column, kind, name="idx")

    theirs = lance.dataset(ours.uri)
    [desc] = [d for d in theirs.describe_indices() if d.name == "idx"]
    assert len(desc.segments) == 1 and set(desc.segments[0].fragment_ids) == set(range(FRAGMENTS))
    for flt in filters:
        assert "ScalarIndexQuery" in theirs.scanner(filter=flt).explain_plan()
        assert _rows(theirs, flt) == _rows(whole, flt) == _rows(ours, flt)


@pytest.mark.parametrize("kind", sorted(SCALAR))
def test_segments_cross_built(lance, make, kind):
    """pylance's segments merged and committed by nanolance; nanolance's committed by pylance."""
    column, filters = SCALAR[kind]
    uri = make("a")
    theirs = lance.dataset(uri)
    segments = [theirs.create_index_uncommitted(column=column, index_type=kind, name="idx", fragment_ids=g)
                for g in _groups(theirs)]
    ours = nl.dataset(uri)
    ours.commit_existing_index_segments("idx", column, [ours.merge_existing_index_segments(segments)])
    for flt in filters:
        assert _rows(lance.dataset(uri), flt) == _rows(ours, flt)
        assert "ScalarIndexQuery" in lance.dataset(uri).scanner(filter=flt).explain_plan()

    uri = make("b")
    ours = nl.dataset(uri)
    segments = [ours.create_index_uncommitted(column, kind, name="idx", fragment_ids=g) for g in _groups(ours)]
    theirs = lance.dataset(uri)
    theirs = theirs.commit_existing_index_segments("idx", column, [_pylance_index(lance, s) for s in segments])
    assert len([d for d in theirs.describe_indices() if d.name == "idx"][0].segments) == 2
    for flt in filters:
        assert _rows(theirs, flt) == _rows(nl.dataset(uri), flt)


def test_inverted_segments_score_as_one_index(lance, make):
    ours = nl.dataset(make("ours"))
    segments = [ours.create_index_uncommitted("text", "INVERTED", name="fts", fragment_ids=g,
                                              with_position=True, remove_stop_words=False)
                for g in _groups(ours)]
    ours.commit_existing_index_segments("fts", "text", [ours.merge_existing_index_segments(segments)])
    whole = lance.dataset(make("whole"))
    whole.create_scalar_index("text", "INVERTED", name="fts", with_position=True, remove_stop_words=False)
    theirs = lance.dataset(ours.uri)
    for query in ("apple", "kiwi melon", "fig"):
        assert _fts(theirs, query) == _fts(whole, query) == _fts(ours, query)
    phrase = lance.query.PhraseQuery("plum fig", "text")
    assert _fts(theirs, phrase) == _fts(whole, phrase)


def test_inverted_legacy_partial_build(lance, make):
    """create_scalar_index(fragment_ids, index_uuid) per fragment, merge_index_metadata, then
    LanceDataset.commit(CreateIndex): Lance's older distributed FTS flow."""
    ours = nl.dataset(make("ours"))
    index_id = str(uuid.uuid4())
    for fragment in ours.get_fragments():
        ours.create_scalar_index("text", "INVERTED", name="fts", replace=False, index_uuid=index_id,
                                 fragment_ids=[fragment.fragment_id], remove_stop_words=False)
    assert ours.list_indices() == []
    events = []
    ours.merge_index_metadata(index_id, index_type="INVERTED", progress_callback=events.append)
    assert [e.stage for e in events if e.event == "complete"] == [
        "read_partition_metadata", "remap_partition_files", "write_merged_metadata"]
    from nanolance.lance.dataset import Index

    from nanolance import _nanolance

    text_id = next(f["id"] for f in _nanolance._ds_fields(ours.uri, None) if f["name"] == "text")
    index = Index(uuid=index_id, name="fts", fields=[text_id],
                  dataset_version=ours.version, fragment_ids=set(range(FRAGMENTS)), index_version=0)
    committed = nl.LanceDataset.commit(ours.uri, nl.LanceOperation.CreateIndex([index], []),
                                       read_version=ours.version)
    assert committed.stats.index_stats("fts")["index_type"] == "Inverted"
    whole = lance.dataset(make("whole"))
    whole.create_scalar_index("text", "INVERTED", name="fts", remove_stop_words=False)
    for query in ("apple", "grape peach"):
        assert _fts(lance.dataset(ours.uri), query) == _fts(whole, query) == _fts(committed, query)


def _nearest(ds, q, k=10, **kw):
    t = ds.to_table(nearest={"column": "vec", "q": q, "k": k, **kw}, columns=["id", "_distance"])
    return t["id"].to_pylist(), np.round(t["_distance"].to_numpy(), 4).tolist()


@pytest.mark.parametrize("kind", ["IVF_FLAT", "IVF_PQ", "IVF_HNSW_SQ"])
def test_vector_segments_with_a_shared_model(lance, make, kind):
    ours = nl.dataset(make("ours"))
    builder = IndicesBuilder(ours, "vec")
    if kind == "IVF_PQ":
        model = builder.prepare_global_ivf_pq(4, 4, sample_rate=2, max_iters=10)
        params = {"num_sub_vectors": 4, **model}
    else:
        params = {"ivf_centroids": builder.train_ivf(4, sample_rate=8, max_iters=10).centroids}
    segments = [ours.create_index_uncommitted("vec", kind, name="vec_idx", num_partitions=4, fragment_ids=g,
                                              **params) for g in _groups(ours)]
    merged = ours.merge_existing_index_segments(segments)
    assert set(merged.fragment_ids) == set(range(FRAGMENTS))
    ours.commit_existing_index_segments("vec_idx", "vec", [merged])
    # pylance builds the same segments with the same model on a copy and commits them unmerged.
    whole = lance.dataset(make("whole"))
    their_segments = [whole.create_index_uncommitted(column="vec", index_type=kind, name="vec_idx",
                                                     num_partitions=4, fragment_ids=g, **params)
                      for g in _groups(whole)]
    whole = whole.commit_existing_index_segments("vec_idx", "vec", their_segments)
    theirs = lance.dataset(ours.uri)
    assert theirs.stats.index_stats("vec_idx")["index_type"] == kind
    rng = np.random.default_rng(11)
    for _ in range(5):
        q = rng.standard_normal(DIM).astype(np.float32) * 3
        exact = _nearest(theirs, q, use_index=False)[0]
        ids, dist = _nearest(theirs, q, nprobes=4, refine_factor=5)
        assert _nearest(ours, q, nprobes=4, refine_factor=5)[0] == ids
        if kind != "IVF_HNSW_SQ":  # the same codes in the same partitions: the same answers
            assert (ids, dist) == _nearest(whole, q, nprobes=4, refine_factor=5)
        assert len(set(ids) & set(exact)) >= 7


def test_vector_merge_needs_one_model(lance, make):
    ours = nl.dataset(make("ours"))
    groups = _groups(ours)
    segments = [ours.create_index_uncommitted("vec", "IVF_FLAT", name="vec_idx", num_partitions=4,
                                              fragment_ids=g) for g in groups]  # each trains its own
    with pytest.raises(RuntimeError, match="IVF centroids mismatch across shards"):
        ours.merge_existing_index_segments(segments)
    # Committed side by side they are one logical index of two segments, searched by both libraries.
    ours.commit_existing_index_segments("vec_idx", "vec", segments)
    theirs = lance.dataset(ours.uri)
    assert len([d for d in theirs.describe_indices() if d.name == "vec_idx"][0].segments) == 2
    q = np.random.default_rng(5).standard_normal(DIM).astype(np.float32)
    assert _nearest(theirs, q, nprobes=4)[0] == _nearest(ours, q, nprobes=4)[0]
    # One segment merges to itself.
    single = ours.merge_existing_index_segments(segments[:1])
    assert single.uuid == segments[0].uuid


def test_trained_models_load_both_ways(lance, make, tmp_path):
    from lance.indices import IvfModel as TheirIvf
    from lance.indices import PqModel as TheirPq

    ours = nl.dataset(make("ours"))
    ivf = IndicesBuilder(ours, "vec").train_ivf(8, distance_type="cosine", sample_rate=4, max_iters=5)
    assert ivf.num_partitions == 8 and ivf.distance_type == "cosine"
    pq = IndicesBuilder(ours, "vec").train_pq(ivf, 4, sample_rate=2, max_iters=5)
    assert (pq.num_subvectors, pq.num_bits, pq.dimension, len(pq.codebook)) == (4, 8, DIM, 256)
    ivf.save(str(tmp_path / "ivf"))
    pq.save(str(tmp_path / "pq"))
    assert TheirIvf.load(str(tmp_path / "ivf")).centroids == ivf.centroids
    their_pq = TheirPq.load(str(tmp_path / "pq"))
    assert their_pq.codebook == pq.codebook and their_pq.num_subvectors == 4

    theirs = lance.dataset(ours.uri)
    their_ivf = lance.indices.IndicesBuilder(theirs, "vec").train_ivf(6, sample_rate=4, max_iters=5)
    their_ivf.save(str(tmp_path / "their_ivf"))
    loaded = IvfModel.load(str(tmp_path / "their_ivf"))
    assert loaded.centroids == their_ivf.centroids and loaded.distance_type == "l2"
    # A pylance-trained PQ model drives a nanolance segment build.
    their_pq = lance.indices.IndicesBuilder(theirs, "vec").train_pq(their_ivf, 4, sample_rate=2, max_iters=5)
    their_pq.save(str(tmp_path / "their_pq"))
    loaded_pq = PqModel.load(str(tmp_path / "their_pq"))
    segment = ours.create_index_uncommitted("vec", "IVF_PQ", num_partitions=6, num_sub_vectors=4,
                                            fragment_ids=[0, 1, 2, 3], ivf_centroids=loaded.centroids,
                                            pq_codebook=loaded_pq.codebook)
    ours.commit_existing_index_segments("vec_idx", "vec", [segment])
    q = np.ones(DIM, np.float32)
    assert _nearest(lance.dataset(ours.uri), q, nprobes=6)[0] == _nearest(ours, q, nprobes=6)[0]


@pytest.mark.parametrize("dtype", [np.float16, np.float32, np.float64])
def test_training_on_any_float_width(tmp_path, dtype):
    values = np.random.default_rng(1).standard_normal((600, 8)).astype(dtype)
    vec = pa.FixedSizeListArray.from_arrays(pa.array(values.reshape(-1)), 8)
    ds = nl.write_dataset(pa.table({"vec": vec}), str(tmp_path / "d"), max_rows_per_file=200)
    builder = IndicesBuilder(ds, "vec")
    ivf = builder.train_ivf(sample_rate=2, max_iters=3)
    assert ivf.num_partitions == round(np.sqrt(600))
    first = builder.train_ivf(2, sample_rate=2, max_iters=3, fragment_ids=[0])
    assert first.num_partitions == 2
    pq = builder.train_pq(ivf, 2, sample_rate=2, max_iters=3, num_bits=4)
    assert len(pq.codebook) == 16 and pq.num_bits == 4


def test_segment_build_errors(make):
    ds = nl.dataset(make("d"))
    with pytest.raises(ValueError, match="create_index_uncommitted"):
        ds.create_scalar_index("id", "BTREE", fragment_ids=[0])
    with pytest.raises(ValueError, match="index_uuid is no longer accepted for BTree"):
        ds.create_index_uncommitted("id", "BTREE", fragment_ids=[0], index_uuid=str(uuid.uuid4()))
    with pytest.raises(ValueError, match="requires fragment_ids"):
        ds.create_index_uncommitted("id", "BITMAP")
    with pytest.raises(ValueError, match="Invalid UUID"):
        ds.create_index_uncommitted("cat", "BITMAP", fragment_ids=[0], index_uuid="nope")
    with pytest.raises(ValueError, match="does not exist in dataset version"):
        ds.create_index_uncommitted("cat", "BITMAP", fragment_ids=[99])
    with pytest.raises(ValueError, match="no longer supports merge_index_metadata"):
        ds.merge_index_metadata(str(uuid.uuid4()), "BTREE")
    one = ds.create_index_uncommitted("cat", "BITMAP", name="b", fragment_ids=[0, 1])
    two = ds.create_index_uncommitted("cat", "BITMAP", name="b", fragment_ids=[1, 2])
    with pytest.raises(ValueError, match="overlapping fragment coverage"):
        ds.merge_existing_index_segments([one, two])
    other = ds.create_index_uncommitted("id", "BTREE", name="b", fragment_ids=[3])
    with pytest.raises(ValueError, match="identical fields|keyed field"):
        ds.merge_existing_index_segments([one, other])
