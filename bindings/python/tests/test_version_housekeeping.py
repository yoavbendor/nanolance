"""Tags, cleanup of old versions, automatic cleanup and drop, against pylance.

The cleanup oracle: the same dataset (copied with its file times) is explained by pylance and by
nanolance, and both must pick the same files, of the same kinds, with the same stats. Then the
cleanup runs and pylance must read every version left exactly as before. Tags go both ways: each
library sees the other's, and each one's cleanup keeps the other's tagged versions.
"""

from __future__ import annotations

import os
import shutil
import time
from datetime import timedelta

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


def _candidates(explanation, root):
    root = os.path.abspath(root).lstrip("/")
    out = set()
    for f in explanation.candidate_files:
        path = f.path.lstrip("/")
        path = path[len(root):].lstrip("/") if path.startswith(root) else path
        out.add((path, f.kind, bool(f.unverified)))
    return out


def _stats(s):
    return (s.bytes_removed, s.old_versions, s.data_files_removed, s.transaction_files_removed,
            s.index_files_removed, s.deletion_files_removed)


def _history(lib, path):
    """A dataset with overwrites, appends, deletes, an update, indexes and a compaction."""
    t = pa.table({"id": range(200), "v": [float(i) for i in range(200)], "s": [f"s{i % 7}" for i in range(200)]})
    lib.write_dataset(t, path, max_rows_per_file=50)
    ds = lib.dataset(path)
    ds.delete("id % 11 = 0")
    ds = lib.dataset(path)
    ds.create_scalar_index("id", "BTREE")
    ds = lib.dataset(path)
    ds.update({"v": "v + 1"}, where="id < 30")
    lib.write_dataset(t.slice(0, 40), path, mode="append")
    ds = lib.dataset(path)
    ds.optimize.compact_files(target_rows_per_fragment=500)
    lib.write_dataset(t.slice(0, 120), path, mode="overwrite", max_rows_per_file=60)
    lib.dataset(path).delete("id > 100")
    return lib.dataset(path)


@pytest.mark.parametrize("writer", ["nanolance", "pylance"])
@pytest.mark.parametrize("policy", [
    {"older_than": timedelta(0)},
    {"retain_versions": 3},
    {"versions": [2, 4, 5]},
    {"older_than": timedelta(0), "delete_unverified": True},
    {"retain_versions": 2, "older_than": timedelta(0)},
])
def test_cleanup_matches_pylance(tmp_path, writer, policy):
    lance = require_pylance()
    src = str(tmp_path / "src.lance")
    _history(nl if writer == "nanolance" else lance, src)
    # A stray file no version names (a write in progress, or one that died): kept unless unverified.
    with open(os.path.join(src, "data", "orphan-0000.lance"), "wb") as f:
        f.write(b"x" * 64)
    ours, theirs = str(tmp_path / "ours.lance"), str(tmp_path / "theirs.lance")
    shutil.copytree(src, ours)
    shutil.copytree(src, theirs)
    time.sleep(0.01)
    kw = dict(policy, error_if_tagged_old_versions=False)
    mine = nl.dataset(ours).explain_cleanup_old_versions(include_files=True, **kw)
    pylance_view = lance.dataset(theirs).explain_cleanup_old_versions(include_files=True, **kw)
    assert _candidates(mine, ours) == _candidates(pylance_view, theirs)
    assert _stats(mine.stats) == _stats(pylance_view.stats)

    versions_before = {v["version"]: lance.dataset(src, version=v["version"]).to_table()
                       for v in lance.dataset(src).versions()}
    stats = nl.dataset(ours).cleanup_old_versions(**kw)
    assert _stats(stats) == _stats(mine.stats)
    left = [v["version"] for v in lance.dataset(ours).versions()]
    assert left and len(left) == len(versions_before) - stats.old_versions
    for v in left:  # every version left reads in full, as before, by pylance and by nanolance
        assert lance.dataset(ours, version=v).to_table().equals(versions_before[v])
        assert nl.dataset(ours, version=v).to_table().equals(versions_before[v])
    # Nothing left for pylance's own cleanup to find that ours missed.
    again = lance.dataset(ours).explain_cleanup_old_versions(include_files=True, **kw)
    assert again.stats.old_versions == 0 and not any(k == "data" and not u for _, k, u in _candidates(again, ours))


def test_unverified_files_by_age(tmp_path):
    path = str(tmp_path / "d.lance")
    nl.write_dataset(pa.table({"a": [1]}), path)
    stray = os.path.join(path, "data", "stray.lance")
    with open(stray, "wb") as f:
        f.write(b"partial")
    nl.dataset(path).cleanup_old_versions(older_than=timedelta(0))
    assert os.path.exists(stray)  # may belong to a write in progress
    week = time.time() - 8 * 86400
    os.utime(stray, (week, week))
    assert nl.dataset(path).cleanup_old_versions(older_than=timedelta(0)).data_files_removed == 1
    assert not os.path.exists(stray)
    assert nl.dataset(path).to_table().num_rows == 1


def test_tags_both_ways(tmp_path):
    lance = require_pylance()
    path = str(tmp_path / "t.lance")
    for i in range(4):
        nl.write_dataset(pa.table({"a": [i]}), path, mode="create" if i == 0 else "overwrite")
    ds = nl.dataset(path)
    ds.tags.create("first", 1)
    ds.tags.replace_metadata("first", {"why": "baseline"})
    lance.dataset(path).tags.create("second", 2)
    assert lance.dataset(path).tags.list()["first"]["metadata"] == {"why": "baseline"}
    assert nl.dataset(path).tags.get_version("second") == 2
    ours, theirs = nl.dataset(path).tags.list(), lance.dataset(path).tags.list()
    assert ours.keys() == theirs.keys()
    for name in ours:
        assert {k: ours[name][k] for k in ("branch", "version", "manifest_size", "metadata", "created_at")} == \
               {k: theirs[name][k] for k in ("branch", "version", "manifest_size", "metadata", "created_at")}
    assert nl.dataset(path, version="second").to_table().equals(lance.dataset(path, version="second").to_table())
    # Each library's cleanup keeps the other's tagged versions.
    with pytest.raises(OSError):
        nl.dataset(path).cleanup_old_versions(older_than=timedelta(0))
    nl.dataset(path).cleanup_old_versions(older_than=timedelta(0), error_if_tagged_old_versions=False)
    assert [v["version"] for v in lance.dataset(path).versions()] == [1, 2, 4]
    assert lance.dataset(path, version="first").to_table().equals(pa.table({"a": [0]}))
    nl.dataset(path).tags.update("first", 4)
    lance.dataset(path).tags.delete("second")
    lance.dataset(path).cleanup_old_versions(older_than=timedelta(0))
    assert [v["version"] for v in nl.dataset(path).versions()] == [4]


@pytest.mark.parametrize("name", ["", ".x", "x.", "a..b", "x.lock", "a/b", "a b"])
def test_tag_names(tmp_path, name):
    path = str(tmp_path / "n.lance")
    nl.write_dataset(pa.table({"a": [1]}), path)
    with pytest.raises(ValueError):
        nl.dataset(path).tags.create(name, 1)


def test_auto_cleanup_on_commit(tmp_path):
    lance = require_pylance()
    path = str(tmp_path / "a.lance")
    nl.write_dataset(pa.table({"a": [0]}), path, auto_cleanup_options={"interval": 2, "older_than_seconds": 0})
    assert nl.dataset(path).config()["lance.auto_cleanup.older_than"] == "0s"
    assert lance.dataset(path).config() == nl.dataset(path).config()
    theirs = str(tmp_path / "p.lance")
    lance.write_dataset(pa.table({"a": [0]}), theirs, auto_cleanup_options={"interval": 2, "older_than_seconds": 0})
    for i in range(1, 5):
        nl.write_dataset(pa.table({"a": [i]}), path, mode="append")
        lance.write_dataset(pa.table({"a": [i]}), theirs, mode="append")
    # Every second version cleans up as of the version before (v4 keeps v3), as pylance does.
    assert [v["version"] for v in nl.dataset(path).versions()] == [3, 4, 5]
    assert [v["version"] for v in lance.dataset(theirs).versions()] == [3, 4, 5]
    assert nl.dataset(path).to_table().equals(lance.dataset(path).to_table())
    assert nl.dataset(path).to_table().num_rows == 5


def test_drop(tmp_path):
    lance = require_pylance()
    parent = tmp_path / "warehouse"
    nl.write_dataset(pa.table({"a": [1]}), str(parent / "t.lance"))
    with pytest.raises(ValueError, match="no readable Lance manifest"):
        nl.LanceDataset.drop(str(parent))
    (parent / "t.lance" / "notes.txt").write_text("kept beside it")
    nl.LanceDataset.drop(str(parent / "t.lance"))
    assert not (parent / "t.lance").exists() and parent.exists()
    nl.LanceDataset.drop(str(parent / "t.lance"), ignore_not_found=True)
    with pytest.raises(OSError):
        nl.LanceDataset.drop(str(parent / "t.lance"))
    lance.write_dataset(pa.table({"a": [1]}), str(parent / "p.lance"))
    nl.LanceDataset.drop(str(parent / "p.lance"))
    assert not (parent / "p.lance").exists()
