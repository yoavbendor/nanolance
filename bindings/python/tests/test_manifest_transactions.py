"""Pylance 12's inline transactions (manifest field 21) and manifest migration (V1 to V2 names), against
pylance: each dataset is written by one library and read, committed to and migrated by both.
"""

from __future__ import annotations

import glob
import os
import re
from pathlib import Path

import pyarrow as pa

import nanolance.lance as nl
from tests.support import require_pylance

_V2_NAME = re.compile(r"\d{20}\.manifest")


def _table(n=10):
    return pa.table({"id": pa.array(range(n), pa.int64())})


def _version_names(uri: Path) -> list[str]:
    return sorted(name for name in os.listdir(uri / "_versions") if name.endswith(".manifest"))


def _drop_transaction_files(uri: Path) -> None:
    # Pylance 12 writes the transaction inline in each manifest too; with the files gone the manifests
    # name files that do not exist.
    for name in glob.glob(str(uri / "_transactions" / "*.txn")):
        os.remove(name)


def test_inline_transactions_read_without_transaction_files(tmp_path: Path):
    lance = require_pylance()
    uri = tmp_path / "ds"
    lance.write_dataset(_table(), uri)
    lance.write_dataset(_table(), uri, mode="append")
    lance.dataset(uri).delete("id < 3")
    _drop_transaction_files(uri)

    pylance_ds = lance.dataset(uri)
    ours_ds = nl.dataset(uri)
    for version in (1, 2, 3):
        theirs = pylance_ds.read_transaction(version)
        ours = ours_ds.read_transaction(version)
        assert ours is not None, f"version {version}: nanolance reads no transaction"
        assert ours.read_version == theirs.read_version
        assert type(ours.operation).__name__ == type(theirs.operation).__name__


def test_delete_raced_by_restore_is_decided_by_the_inline_transaction(tmp_path: Path):
    lance = require_pylance()
    uri = tmp_path / "ds"
    lance.write_dataset(_table(), uri)
    lance.write_dataset(_table(), uri, mode="append")
    lance.dataset(uri, version=1).restore()  # version 3: a Restore
    _drop_transaction_files(uri)

    # Ours is made from version 2 and races the Restore: incompatible, so not run again. Without the
    # inline transaction the version would read as one without a transaction file (retryable).
    ours = nl.dataset(uri, version=2)
    try:
        ours.delete("id = 5")
    except OSError as error:
        message = str(error)
    else:
        raise AssertionError("the delete was committed over a restore")
    assert message.startswith("Incompatible transaction: This Delete transaction is incompatible with "
                              "concurrent transaction Restore at version 3."), message
    assert nl.dataset(uri).version == 3


def test_delete_raced_by_append_is_rebased_over_the_inline_transaction(tmp_path: Path):
    lance = require_pylance()
    uri = tmp_path / "ds"
    lance.write_dataset(_table(), uri)
    lance.write_dataset(_table(), uri, mode="append")  # version 2: an Append
    _drop_transaction_files(uri)

    # Ours is made from version 1 and races the Append: compatible, so rebased onto version 2.
    nl.dataset(uri, version=1).delete("id = 0")
    assert nl.dataset(uri).version == 3
    # The delete saw version 1 only: its one row with id 0 goes; the appended row with id 0 stays.
    assert nl.dataset(uri).count_rows() == 19
    assert lance.dataset(uri).count_rows() == 19


def test_v1_manifest_names_migrate_and_both_engines_open(tmp_path: Path):
    lance = require_pylance()
    uri = tmp_path / "ds"
    lance.write_dataset(_table(), uri, enable_v2_manifest_paths=False)
    assert _version_names(uri) == ["1.manifest"]

    nl.dataset(uri).migrate_manifest_paths_v2()
    names = _version_names(uri)
    assert len(names) == 1 and _V2_NAME.fullmatch(names[0]), names
    assert nl.dataset(uri).version == 1
    assert nl.dataset(uri).count_rows() == 10
    assert lance.dataset(uri).count_rows() == 10

    # Committing after the migration keeps the V2 names and both engines still read the dataset.
    nl.write_dataset(_table(3), uri, mode="append")
    assert all(_V2_NAME.fullmatch(name) for name in _version_names(uri))
    assert lance.dataset(uri).count_rows() == 13


def test_manifest_needs_migration_is_false_for_current_writers(tmp_path: Path):
    lance = require_pylance()
    from nanolance.lance.lance import manifest_needs_migration

    lance.write_dataset(_table(), tmp_path / "pylance")
    assert not manifest_needs_migration(nl.dataset(tmp_path / "pylance"))
    nl.write_dataset(_table(), tmp_path / "nanolance")
    assert not manifest_needs_migration(nl.dataset(tmp_path / "nanolance"))
