"""cognee's LanceDB workload (topoteretes/cognee 525b1d1: LanceDBAdapter and cognee_db_workers), on nanolance.

cognee keeps one table per collection: `id` string, `vector` fixed_size_list<float32>, `payload` struct holding a
`belongs_to_set` list<string>. It never builds a vector index -- every search is an exact cosine scan, prefiltered --
and it compacts with the step-by-step API (`from lance.optimize import Compaction, CompactionOptions`), bounded to a
few tasks, then deletes chosen versions by number. Each step here gives the same answer on nanolance as on pylance.
"""

from __future__ import annotations

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

D = 64


def _payload_type():
    return pa.struct([pa.field("id", pa.string()), pa.field("text", pa.string()),
                      pa.field("belongs_to_set", pa.list_(pa.field("item", pa.string()))),
                      pa.field("type", pa.string())])


SCHEMA = pa.schema([pa.field("id", pa.string()), pa.field("vector", pa.list_(pa.field("item", pa.float32()), D)),
                    pa.field("payload", _payload_type())])
QUERY = np.random.default_rng(99).standard_normal(D).astype("float32")


def _rows(lo, hi):
    ids = [f"id-{i}" for i in range(lo, hi)]
    vectors = np.random.default_rng(lo).standard_normal((hi - lo, D)).astype("float32")
    payload = [{"id": x, "text": f"chunk {x}", "belongs_to_set": ["abc"[i % 3]] if i % 5 else [],
                "type": "DocumentChunk"} for i, x in enumerate(ids)]
    return pa.table([pa.array(ids), pa.FixedSizeListArray.from_arrays(pa.array(vectors.ravel()), D),
                     pa.array(payload, _payload_type())], schema=SCHEMA)


def _search(mod, path, k, where=None):
    t = mod.dataset(path).to_table(nearest={"column": "vector", "q": QUERY, "k": k, "metric": "cosine"},
                                   filter=where, prefilter=True, columns=["id", "_distance"])
    return [(i, round(d, 5)) for i, d in zip(t.column("id").to_pylist(), t.column("_distance").to_pylist())]


def _workload(mod, path):
    from importlib import import_module
    optimize = import_module(mod.__name__ + ".optimize")
    out = {}
    mod.write_dataset(SCHEMA.empty_table(), path, mode="create")  # create_table(schema=..., exist_ok=True)
    for k in range(12):  # add_data_points: one small append per batch
        mod.write_dataset(_rows(k * 50, k * 50 + 50), path, mode="append")
    r = (mod.dataset(path).merge_insert("id").when_matched_update_all().when_not_matched_insert_all()
         .execute(_rows(575, 650)))
    out["upsert"] = (r["num_inserted_rows"], r["num_updated_rows"])
    out["update_only"] = mod.dataset(path).merge_insert("id").when_matched_update_all().execute(_rows(0, 5))[
        "num_updated_rows"]
    ds = mod.dataset(path)
    out["by_id"] = sorted(ds.to_table(filter="id IN ('id-3', 'id-7', 'id-649')").column("id").to_pylist())
    out["any"] = ds.to_table(filter="array_has_any(payload.belongs_to_set, ['a'])").num_rows
    out["all"] = ds.to_table(filter="array_has_all(payload.belongs_to_set, ['a', 'b'])").num_rows
    out["any_and_ids"] = ds.to_table(
        filter="(array_has_any(payload.belongs_to_set, ['b'])) AND id IN ('id-1', 'id-4', 'id-5')"
    ).column("id").to_pylist()
    out["top10"] = _search(mod, path, 10)
    out["top10_in_set"] = _search(mod, path, 10, "array_has_any(payload.belongs_to_set, ['c'])")
    out["score_by_ids"] = sorted(_search(mod, path, 3, "id IN ('id-1', 'id-2', 'id-3')"))
    out["full_ranking"] = len(_search(mod, path, ds.count_rows()))
    mod.dataset(path).delete("id IN ('id-0', 'id-1', 'id-2')")
    out["after_delete"] = mod.dataset(path).count_rows()
    # cognee_db_workers.lancedb_compaction.compact_fragments, bounded to 4 tasks
    ds = mod.dataset(path)
    options = optimize.CompactionOptions(target_rows_per_fragment=20_000, materialize_deletions=True,
                                         materialize_deletions_threshold=0.2)
    plan = optimize.Compaction.plan(ds, options)
    metrics = optimize.Compaction.commit(ds, [task.execute(ds) for task in list(plan.tasks)[:4]])
    out["compaction"] = (plan.num_tasks(), int(metrics.fragments_removed), int(metrics.fragments_added),
                         len(mod.dataset(path).get_fragments()))
    # prune_superseded_versions: versions deleted by number, tags not raised on
    ds = mod.dataset(path)
    versions = [int(v["version"]) for v in ds.versions()]
    cleanup = ds.cleanup_old_versions(versions=versions[:-1][:5], error_if_tagged_old_versions=False)
    out["cleanup"] = (int(cleanup.old_versions), len(mod.dataset(path).versions()))
    out["rows"] = mod.dataset(path).to_table().sort_by("id").to_pylist()
    return out


def test_compaction_api_importable_from_optimize():
    from nanolance.lance.lance import Compaction as from_lance
    from nanolance.lance.optimize import Compaction, CompactionOptions, CompactionPlan, CompactionTask

    assert Compaction is from_lance and CompactionPlan and CompactionTask
    assert CompactionOptions(target_rows_per_fragment=10) == {"target_rows_per_fragment": 10}


def test_same_answers_as_pylance(tmp_path):
    lance = require_pylance()
    ours, theirs = str(tmp_path / "ours.lance"), str(tmp_path / "theirs.lance")
    mine, pylance = _workload(nl, ours), _workload(lance, theirs)
    for key in pylance:
        assert mine[key] == pylance[key], key
    assert mine["compaction"][3] == 1 and mine["upsert"] == (50, 25)
    # Each library reads the other's table.
    assert lance.dataset(ours).count_rows() == nl.dataset(theirs).count_rows() == mine["after_delete"]


@pytest.mark.parametrize("where", [None, "array_has_any(payload.belongs_to_set, ['a'])"])
def test_flat_cosine_is_exact(tmp_path, where):
    """No index: the top k is the exact cosine top k over the (filtered) rows."""
    path = str(tmp_path / "ds.lance")
    nl.write_dataset(_rows(0, 400), path)
    table = nl.dataset(path).to_table(filter=where)
    vectors = np.stack(table.column("vector").to_numpy(zero_copy_only=False))
    distances = 1 - vectors @ QUERY / (np.linalg.norm(vectors, axis=1) * np.linalg.norm(QUERY))
    want = [table.column("id")[int(i)].as_py() for i in np.argsort(distances)[:10]]
    assert [i for i, _ in _search(nl, path, 10, where)] == want
