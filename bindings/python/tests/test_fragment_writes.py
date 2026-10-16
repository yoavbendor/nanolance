"""Fragment-level writes and hand-built transactions, against pylance.

A distributed write as Ray / Daft / Spark make it: workers write fragments apart from any commit
(write_fragments, LanceFragment.create, delete, merge_columns, update_columns), then one commit
publishes them (LanceDataset.commit with a LanceOperation). Each flow runs on both libraries and
must give the same data; the datasets nanolance commits must read in pylance (and validate there), the
transactions they record must be the operation committed; fragments written by one library commit in
the other; and conflicts between concurrent transactions come out as Lance decides them.
"""

from __future__ import annotations

import json

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


def _table(lo, hi):
    return pa.table({"id": pa.array(range(lo, hi), pa.int64()), "s": [f"v{i}" for i in range(lo, hi)]})


def _flow(lib, path):
    """Create, append, delete, merge new columns, update columns, compact by hand, restore: the
    tables after each step."""
    out = []
    frags = lib.fragment.write_fragments(_table(0, 300), path, max_rows_per_file=100)
    ds = lib.LanceDataset.commit(path, lib.LanceOperation.Overwrite(_table(0, 1).schema, frags))
    out.append(ds.to_table())
    more = [lib.LanceFragment.create(path, _table(300, 350))]
    ds = lib.LanceDataset.commit(path, lib.LanceOperation.Append(more), read_version=ds.version)
    out.append(ds.to_table())
    fragments = ds.get_fragments()
    first = fragments[0].delete("id % 7 = 0")
    assert fragments[1].delete("id >= 0") is None  # every row: the fragment goes
    ds = lib.LanceDataset.commit(path, lib.LanceOperation.Delete([first], [fragments[1].fragment_id], "x"),
                                 read_version=ds.version)
    out.append(ds.to_table())
    merged, schema = [], None
    for f in ds.get_fragments():
        m, schema = f.merge_columns(lambda b: pa.record_batch([pa.compute.multiply(b["id"], 2)], ["twice"]),
                                    columns=["id"])
        merged.append(m)
    ds = lib.LanceDataset.commit(path, lib.LanceOperation.Merge(merged, schema), read_version=ds.version)
    out.append(ds.to_table())
    frag = ds.get_fragments()[-1]
    updated, modified = frag.update_columns(pa.table({"id": pa.array([301, 305], pa.int64()), "s": ["x", "y"]}),
                                            left_on="id")
    ds = lib.LanceDataset.commit(path, lib.LanceOperation.Update(updated_fragments=[updated],
                                                                 fields_modified=modified),
                                 read_version=ds.version)
    out.append(ds.to_table())
    ds = lib.LanceDataset.commit(path, lib.LanceOperation.Restore(2))
    out.append(ds.to_table())
    return out


def test_flow_matches_pylance(tmp_path):
    lance = require_pylance()
    mine = _flow(nl, str(tmp_path / "nl"))
    theirs = _flow(lance, str(tmp_path / "pl"))
    assert len(mine) == len(theirs)
    for a, b in zip(mine, theirs):
        assert a.equals(b), (a, b)
    # pylance reads nanolance's commits, finds them valid, and reads back the operations committed.
    ds = lance.dataset(str(tmp_path / "nl"))
    ds.validate()
    assert ds.to_table().equals(mine[-1])
    kinds = [type(ds.read_transaction(v).operation).__name__ for v in range(1, ds.version + 1)]
    assert kinds == ["Overwrite", "Append", "Delete", "Merge", "Update", "Restore"]
    for v in range(1, ds.version + 1):
        assert lance.dataset(str(tmp_path / "nl"), version=v).to_table().equals(
            lance.dataset(str(tmp_path / "pl"), version=v).to_table())


def test_fragments_cross_libraries(tmp_path):
    """Fragments written by one library, passed on as JSON (what a distributed job ships between
    workers), committed by the other."""
    lance = require_pylance()
    for writer, committer in ((lance, nl), (nl, lance)):
        path = str(tmp_path / f"{writer.__name__}-{committer.__name__}")
        frags = writer.fragment.write_fragments(_table(0, 50), path)
        shipped = [committer.FragmentMetadata.from_json(json.dumps(f.to_json())) for f in frags]
        ds = committer.LanceDataset.commit(path, committer.LanceOperation.Overwrite(_table(0, 1).schema, shipped))
        frags = writer.fragment.write_fragments(_table(50, 80), path)
        shipped = [committer.FragmentMetadata.from_json(json.dumps(f.to_json())) for f in frags]
        ds = committer.LanceDataset.commit(path, committer.LanceOperation.Append(shipped), read_version=ds.version)
        for lib in (nl, lance):
            assert lib.dataset(path).to_table().equals(_table(0, 80))


def test_conflicts_as_lance_decides(tmp_path):
    lance = require_pylance()
    for lib in (nl, lance):
        path = str(tmp_path / lib.__name__)
        lib.write_dataset(_table(0, 100), path, max_rows_per_file=50)
        ds = lib.dataset(path)
        a = ds.get_fragment(0).delete("id < 10")
        b = ds.get_fragment(0).delete("id > 40")
        lib.LanceDataset.commit(path, lib.LanceOperation.Delete([a], [], "x"), read_version=ds.version)
        # Two deletes from one fragment: retryable. An append beside a delete: fine.
        with pytest.raises(OSError, match="Retryable commit conflict") as err:
            lib.LanceDataset.commit(path, lib.LanceOperation.Delete([b], [], "y"), read_version=ds.version)
        assert err.value.retryable is True
        append = lib.LanceOperation.Append([lib.LanceFragment.create(path, _table(100, 110))])
        lib.LanceDataset.commit(path, append, read_version=ds.version)
        # An append from before an overwrite: incompatible.
        stale = lib.dataset(path).version
        lib.write_dataset(_table(0, 5), path, mode="overwrite")
        append = lib.LanceOperation.Append([lib.LanceFragment.create(path, _table(5, 6))])
        with pytest.raises(OSError, match="Incompatible transaction") as err:
            lib.LanceDataset.commit(path, append, read_version=stale)
        assert err.value.retryable is False
    assert nl.dataset(str(tmp_path / "nanolance.lance")).to_table().equals(
        lance.dataset(str(tmp_path / "lance")).to_table())


def test_data_replacement_and_rewrite(tmp_path):
    """Replace a column's file, then compact two fragments by hand (Rewrite), in both libraries."""
    lance = require_pylance()
    tables = []
    for lib in (nl, lance):
        path = str(tmp_path / lib.__name__)
        ds = lib.write_dataset(pa.table({"a": range(100)}), path, max_rows_per_file=50)
        ds.add_columns({"b": "a + 1"})
        ds = lib.dataset(path)
        new = lib.LanceFragment.create(path + "-scratch", pa.table({"b": pa.array(range(500, 550), pa.int64())}))
        # The file is moved into the dataset and renumbered to field b's id.
        import os
        import shutil

        shutil.move(os.path.join(path + "-scratch", "data", new.files[0].path), os.path.join(path, "data"))
        replacement = lib.fragment.DataFile(new.files[0].path, [1], [0], new.files[0].file_major_version,
                                   new.files[0].file_minor_version)
        ds = lib.LanceDataset.commit(path, lib.LanceOperation.DataReplacement(
            [lib.LanceOperation.DataReplacementGroup(0, replacement)]), read_version=ds.version)
        old = [f.metadata for f in ds.get_fragments()]
        rewritten = lib.fragment.write_fragments(ds.to_table(), path, max_rows_per_file=1000)
        ds = lib.LanceDataset.commit(path, lib.LanceOperation.Rewrite(
            [lib.LanceOperation.RewriteGroup(old, rewritten)], []), read_version=ds.version)
        assert len(ds.get_fragments()) == 1
        tables.append(ds.to_table())
    assert tables[0].equals(tables[1])
    assert tables[0].column("b").to_pylist()[:50] == list(range(500, 550))


def test_commit_batch_and_uncommitted_merge_insert(tmp_path):
    lance = require_pylance()
    results = []
    for lib in (nl, lance):
        path = str(tmp_path / lib.__name__)
        ds = lib.write_dataset(_table(0, 10), path)
        txns = [lib.Transaction(1, lib.LanceOperation.Append(lib.fragment.write_fragments(_table(lo, lo + 5), ds)))
                for lo in (10, 15)]
        ds = lib.LanceDataset.commit_batch(ds, txns)["dataset"]
        txn, stats = ds.merge_insert("id").when_matched_update_all().when_not_matched_insert_all() \
            .execute_uncommitted(_table(18, 25))
        assert stats == {"num_inserted_rows": 5, "num_updated_rows": 2, "num_deleted_rows": 0}
        assert lib.dataset(path).version == ds.version  # nothing committed yet
        ds = lib.LanceDataset.commit(path, txn)
        results.append(ds.to_table().sort_by("id"))
    assert results[0].equals(results[1])
