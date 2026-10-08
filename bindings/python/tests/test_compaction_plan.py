"""Compaction planning against pylance: the same dataset and options must leave the same fragments
(ids and row counts), at the same version, with the same metrics and rows.

Covers what decides a plan: fragment sizes against the target, deletions against the materialize
threshold, excluded fragments as boundaries, index coverage as a boundary (an indexed fragment is
never combined with an unindexed one), and the source budgets (fragments, rows, bytes).
"""

from __future__ import annotations

import random

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


def _build(lib, path, seed):
    rng = random.Random(seed)
    sizes = [rng.choice([5, 30, 80, 200, 450]) for _ in range(rng.randint(4, 9))]
    start = 0
    for k, n in enumerate(sizes):
        t = pa.table({"id": range(start, start + n), "s": [f"v{i % 13}" for i in range(start, start + n)]})
        lib.write_dataset(t, path, mode="create" if k == 0 else "append")
        start += n
    ds = lib.dataset(path)
    if rng.random() < 0.6:
        ds.delete(f"id % {rng.choice([2, 3, 7])} = 0 AND id < {rng.randint(0, start)}")
    if rng.random() < 0.4:
        lib.dataset(path).create_scalar_index("id", "BTREE")
        lib.write_dataset(pa.table({"id": [10_000, 10_001], "s": ["x", "y"]}), path, mode="append")
    return lib.dataset(path)


def _options(seed, ds):
    rng = random.Random(seed * 31 + 1)
    ids = [f.fragment_id for f in ds.get_fragments()]
    opts = dict(target_rows_per_fragment=rng.choice([100, 300, 1000]),
                materialize_deletions=rng.random() < 0.7,
                materialize_deletions_threshold=rng.choice([0.05, 0.1, 0.5]))
    if rng.random() < 0.3:
        opts["excluded_fragment_ids"] = rng.sample(ids, 1) + [999]
    budget = rng.random()
    if budget < 0.2:
        opts["max_source_fragments"] = rng.randint(1, 4)
    elif budget < 0.4:
        opts["max_source_rows"] = rng.randint(50, 600)
    return opts


@pytest.mark.parametrize("seed", range(16))
def test_compaction_matches_pylance(tmp_path, seed):
    lance = require_pylance()
    ours = _build(nl, str(tmp_path / "n.lance"), seed)
    theirs = _build(lance, str(tmp_path / "p.lance"), seed)
    assert [(f.fragment_id, f.count_rows()) for f in ours.get_fragments()] == \
           [(f.fragment_id, f.count_rows()) for f in theirs.get_fragments()]
    opts = _options(seed, theirs)
    before_ids = {f.fragment_id for f in theirs.get_fragments()}
    m_ours = ours.optimize.compact_files(**opts)
    m_theirs = theirs.optimize.compact_files(**opts)
    ours, theirs = nl.dataset(ours.uri), lance.dataset(theirs.uri)
    assert (m_ours.fragments_removed, m_ours.fragments_added) == (m_theirs.fragments_removed, m_theirs.fragments_added)
    # Fragments kept as they were match exactly; Lance numbers the new ones in the order its tasks
    # finish (buffer_unordered), so those match as sets of ids and of row counts.
    def layout(ds, new):
        frags = [(f.fragment_id, f.count_rows()) for f in ds.get_fragments()]
        kept = [f for f in frags if f[0] < new]
        added = [f for f in frags if f[0] >= new]
        return kept, sorted(i for i, _ in added), sorted(n for _, n in added)

    first_new = min((f.fragment_id for f in theirs.get_fragments()
                     if f.fragment_id not in before_ids), default=1 << 31)
    assert layout(ours, first_new) == layout(theirs, first_new), opts
    assert ours.to_table().sort_by("id").equals(theirs.to_table().sort_by("id"))
    if m_theirs.fragments_removed and not ours.describe_indices():
        assert ours.version == theirs.version  # reserve, then rewrite: two versions, as Lance

        def kinds(path, v):
            ds = lance.dataset(path)
            return [type(ds.read_transaction(k).operation).__name__ for k in (v - 1, v)]

        assert kinds(ours.uri, ours.version) == kinds(theirs.uri, theirs.version)


def test_budgets_refuse_zero(tmp_path):
    ds = nl.write_dataset(pa.table({"a": range(10)}), str(tmp_path / "z.lance"), max_rows_per_file=2)
    for name in ("max_source_fragments", "max_source_rows", "max_source_bytes"):
        with pytest.raises(OSError, match="must be greater than 0"):
            ds.optimize.compact_files(**{name: 0})


def test_max_bytes_per_file(tmp_path):
    data = pa.table({"a": pa.FixedSizeListArray.from_arrays(pa.array(range(1024 * 1024)), 1024)})
    ds = nl.write_dataset(data, str(tmp_path / "b.lance"), max_rows_per_file=512)
    metrics = ds.optimize.compact_files(target_rows_per_fragment=100_000, max_bytes_per_file=1000, batch_size=128)
    assert metrics.fragments_removed == 2 and metrics.fragments_added > 2
    assert len(nl.dataset(ds.uri).get_fragments()) == metrics.fragments_added
    assert nl.dataset(ds.uri).to_table().equals(data)
