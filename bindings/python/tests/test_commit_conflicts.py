# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Lost races decided as Lance 12 decides them (docs/tasks/CONFLICTS.md, C1b).

Each row is a change made from a stale handle (opened at version 1) after another writer committed. The
same row is run through pylance and through nanolance.lance: the outcome (an OSError with pylance's
message, or success) and the row count must agree, except the four rows where pylance surfaces a retryable
conflict and nanolance re-runs the operation, which must still succeed.
"""

from __future__ import annotations

from pathlib import Path

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

# Rows where pylance surfaces a retryable conflict and nanolance re-runs the operation on the latest
# version, which must succeed.
RE_RUN_KEEP = {
    "compaction vs delete",
    "index vs compaction",
    "index vs index (same name)",
    "add_columns vs delete",
}


def _table(values):
    return pa.table({"id": pa.array(values, pa.int64()), "v": pa.array([float(i) for i in values])})


def _fresh(lib, root: Path) -> str:
    """A 100-row dataset of two fragments of 50, under `root` (created)."""
    root.mkdir(parents=True, exist_ok=True)
    uri = str(root / "ds")
    lib.write_dataset(_table(range(100)), uri, max_rows_per_file=50)
    return uri


def _one_row(lib):
    return _table([1000])


# (name, change by another writer, change made from the stale handle). Each takes the library module.
ROWS = [
    ("delete vs delete (same frag, disjoint rows)",
     lambda L, ds: ds.delete("id = 1"), lambda L, ds: ds.delete("id = 2")),
    ("delete vs delete (same row)",
     lambda L, ds: ds.delete("id = 1"), lambda L, ds: ds.delete("id = 1")),
    ("delete vs delete (other frag)",
     lambda L, ds: ds.delete("id = 1"), lambda L, ds: ds.delete("id = 70")),
    ("delete vs overwrite",
     lambda L, ds: L.write_dataset(_one_row(L), ds.uri, mode="overwrite"), lambda L, ds: ds.delete("id = 2")),
    ("update vs delete (same frag)",
     lambda L, ds: ds.delete("id = 1"), lambda L, ds: ds.update({"v": "0.5"}, "id = 2")),
    ("delete vs update (same frag)",
     lambda L, ds: ds.update({"v": "0.5"}, "id = 2"), lambda L, ds: ds.delete("id = 1")),
    ("delete vs compaction",
     lambda L, ds: ds.optimize.compact_files(target_rows_per_fragment=1000), lambda L, ds: ds.delete("id = 2")),
    ("compaction vs delete",
     lambda L, ds: ds.delete("id = 2"), lambda L, ds: ds.optimize.compact_files(target_rows_per_fragment=1000)),
    ("index vs delete",
     lambda L, ds: ds.delete("id = 2"), lambda L, ds: ds.create_scalar_index("id", "BTREE")),
    ("index vs compaction",
     lambda L, ds: ds.optimize.compact_files(target_rows_per_fragment=1000),
     lambda L, ds: ds.create_scalar_index("id", "BTREE")),
    ("index vs index (other col)",
     lambda L, ds: ds.create_scalar_index("v", "BTREE"), lambda L, ds: ds.create_scalar_index("id", "BTREE")),
    ("index vs index (same name)",
     lambda L, ds: ds.create_scalar_index("id", "BTREE"), lambda L, ds: ds.create_scalar_index("id", "BTREE")),
    ("append vs overwrite",
     lambda L, ds: L.write_dataset(_one_row(L), ds.uri, mode="overwrite"),
     lambda L, ds: L.write_dataset(_one_row(L), ds.uri, mode="append")),
    ("add_columns vs delete",
     lambda L, ds: ds.delete("id = 2"), lambda L, ds: ds.add_columns({"w": "v * 2"})),
    ("drop_columns vs append",
     lambda L, ds: L.write_dataset(_one_row(L), ds.uri, mode="append"), lambda L, ds: ds.drop_columns(["v"])),
    ("update_config vs update_config same key",
     lambda L, ds: ds.update_config({"k": "1"}), lambda L, ds: ds.update_config({"k": "2"})),
    ("restore vs append",
     lambda L, ds: L.write_dataset(_one_row(L), ds.uri, mode="append"), lambda L, ds: ds.restore()),
]


def _run(lib, root: Path, name, theirs, ours):
    """(outcome, rows): outcome is None on success, else the error's message as pylance words it."""
    uri = _fresh(lib, root)
    stale = lib.dataset(uri)  # ours: opened at version 1, before the other writer
    theirs(lib, lib.dataset(uri))
    try:
        ours(lib, stale)
    except OSError as err:  # pylance's CommitConflictError and nanolance's OSError both derive from it
        outcome = str(err).split(", /")[0]  # pylance appends its Rust source location
    else:
        outcome = None
    return outcome, lib.dataset(uri).count_rows()


@pytest.fixture(scope="module")
def pylance():
    return require_pylance()


@pytest.mark.parametrize("name,theirs,ours", ROWS, ids=[r[0] for r in ROWS])
def test_probe_row_matches_pylance(pylance, tmp_path, name, theirs, ours):
    slug = name.replace(" ", "_").replace("(", "").replace(")", "").replace(",", "")
    expected_outcome, expected_rows = _run(pylance, tmp_path / "py" / slug, name, theirs, ours)
    got_outcome, got_rows = _run(nl, tmp_path / "nl" / slug, name, theirs, ours)
    assert got_rows == expected_rows, name
    if name in RE_RUN_KEEP:
        # pylance surfaces a retryable conflict here; nanolance re-runs the operation and it succeeds.
        assert expected_outcome is not None and "Retryable commit conflict" in expected_outcome, name
        assert got_outcome is None, name
        return
    assert got_outcome == expected_outcome, name


def test_incompatible_rows_raise_pylance_message(tmp_path):
    """The two rows the probe showed wrong before: pylance's OSError message, through nanolance.lance."""
    d = _fresh(nl, tmp_path / "overwrite")
    stale = nl.dataset(d)
    nl.write_dataset(_one_row(nl), d, mode="overwrite")
    with pytest.raises(OSError) as info:
        stale.delete("id = 2")
    assert str(info.value) == (
        "Incompatible transaction: This Delete transaction is incompatible with concurrent transaction "
        "Overwrite at version 2."
    )

    d = _fresh(nl, tmp_path / "config")
    stale = nl.dataset(d)
    nl.dataset(d).update_config({"k": "1"})
    with pytest.raises(OSError) as info:
        stale.update_config({"k": "2"})
    assert str(info.value) == (
        "Incompatible transaction: This UpdateConfig transaction is incompatible with concurrent transaction "
        "UpdateConfig at version 2."
    )
    assert nl.dataset(d).config()["k"] == "1"  # the refused change is not in the table


def test_pylance_writer_racing_nanolance_writer(pylance, tmp_path):
    """pylance commits from its own stale version and reads nanolance's transaction file."""
    d = _fresh(nl, tmp_path / "pylance_stale")
    stale = pylance.dataset(d)
    nl.dataset(d).update_config({"k": "1"})  # nanolance's transaction file: version 2
    with pytest.raises(OSError, match="Incompatible transaction: This UpdateConfig transaction"):
        stale.update_config({"k": "2"})


def test_nanolance_writer_racing_pylance_writer(pylance, tmp_path):
    """nanolance commits from its own stale version and reads pylance's transaction file."""
    d = _fresh(nl, tmp_path / "nanolance_stale")
    stale = nl.dataset(d)
    pylance.dataset(d).update_config({"k": "1"})  # pylance's transaction file: version 2
    with pytest.raises(OSError, match="Incompatible transaction: This UpdateConfig transaction"):
        stale.update_config({"k": "2"})
    assert nl.dataset(d).config()["k"] == "1"
