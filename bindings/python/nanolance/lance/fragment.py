# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.fragment`` as nanolance implements it: LanceFragment and its metadata records."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Iterator, List, Optional

import pyarrow as pa

from nanolance import _nanolance
from nanolance.lance._errors import native, unsupported

if TYPE_CHECKING:  # pragma: no cover
    from nanolance.lance.dataset import LanceDataset


@dataclass
class DataFile:
    path: str
    fields: List[int]
    column_indices: List[int] = field(default_factory=list)
    file_major_version: int = 2
    file_minor_version: int = 2
    file_size_bytes: Optional[int] = None
    base_id: Optional[int] = None

    def field_ids(self) -> List[int]:
        return list(self.fields)


@dataclass
class DeletionFile:
    path: str
    num_deleted_rows: int

    def asdict(self) -> dict:
        return {"path": self.path, "num_deleted_rows": self.num_deleted_rows}


@dataclass
class FragmentMetadata:
    id: int
    files: List[DataFile]
    physical_rows: int
    deletion_file: Optional[DeletionFile] = None
    row_id_meta: Optional[object] = None
    created_at_version_meta: Optional[object] = None
    last_updated_at_version_meta: Optional[object] = None
    overlays: List[object] = field(default_factory=list)

    @property
    def num_deletions(self) -> int:
        return 0 if self.deletion_file is None else self.deletion_file.num_deleted_rows

    @property
    def num_rows(self) -> int:
        return self.physical_rows - self.num_deletions

    def data_files(self) -> List[DataFile]:
        return list(self.files)

    def to_json(self) -> dict:
        return {
            "id": self.id,
            "files": [vars(f) for f in self.files],
            "physical_rows": self.physical_rows,
            "deletion_file": None if self.deletion_file is None else self.deletion_file.asdict(),
        }


class LanceFragment:
    """One fragment of a dataset version. Mirrors ``lance.LanceFragment``."""

    def __init__(self, dataset: "LanceDataset", fragment_id: Optional[int], *, _info: Optional[dict] = None,
                 **kwargs):
        if fragment_id is None:
            raise unsupported("LanceFragment without a fragment id")
        self._ds = dataset
        self._id = int(fragment_id)
        if _info is None:
            _info = next((f for f in dataset._info["fragments"] if f["id"] == self._id), None)
            if _info is None:
                raise ValueError(f"Fragment {fragment_id} not found")
        self._info = _info

    @property
    def fragment_id(self) -> int:
        return self._id

    @property
    def metadata(self) -> FragmentMetadata:
        files = [
            DataFile(
                path=f["path"],
                fields=list(f["fields"]),
                column_indices=list(range(len(f["fields"]))),
                file_major_version=int(f["major_version"]),
                file_minor_version=int(f["minor_version"]),
                file_size_bytes=int(f["size_bytes"]),
            )
            for f in self._info["files"]
        ]
        deletion = None
        if self._info["deletion_file"]:
            deletion = DeletionFile(self._info["deletion_file"], int(self._info["deleted_rows"]))
        return FragmentMetadata(self._id, files, int(self._info["physical_rows"]), deletion)

    @property
    def physical_rows(self) -> int:
        return int(self._info["physical_rows"])

    @property
    def num_deletions(self) -> int:
        return int(self._info["deleted_rows"])

    def count_rows(self, filter=None) -> int:
        if filter is not None:
            return self.scanner(filter=filter).count_rows()
        return self.physical_rows - self.num_deletions

    @property
    def schema(self) -> pa.Schema:
        return self._ds.schema

    def data_files(self) -> List[DataFile]:
        return self.metadata.files

    def deletion_file(self):
        return self._info["deletion_file"]

    def scanner(self, columns=None, batch_size=None, filter=None, limit=None, offset=None, with_row_id=False,
                with_row_address=False, **kwargs):
        from nanolance.lance.dataset import LanceScanner

        return LanceScanner(self._ds, columns=columns, batch_size=batch_size, filter=filter, limit=limit,
                            offset=offset, fragments=[self._id], with_row_id=with_row_id,
                            with_row_address=with_row_address, **kwargs)

    def to_table(self, columns=None, filter=None, limit=None, offset=None, with_row_id=False,
                 with_row_address=False, **kwargs) -> pa.Table:
        return self.scanner(columns=columns, filter=filter, limit=limit, offset=offset, with_row_id=with_row_id,
                            with_row_address=with_row_address, **kwargs).to_table()

    def to_batches(self, columns=None, batch_size=None, filter=None, limit=None, offset=None, with_row_id=False,
                   **kwargs) -> Iterator[pa.RecordBatch]:
        return self.scanner(columns=columns, batch_size=batch_size, filter=filter, limit=limit, offset=offset,
                            with_row_id=with_row_id, **kwargs).to_batches()

    def head(self, num_rows: int) -> pa.Table:
        return self.to_table(limit=num_rows)

    def take(self, indices, columns=None) -> pa.Table:
        addresses = [(self._id << 32) | int(i) for i in indices]
        return self._ds._take_rows(addresses, columns=columns)

    def __repr__(self) -> str:
        return f"LanceFragment(id={self._id})"

    def __reduce__(self):
        return (_restore_fragment, (self._ds.uri, self._ds.version, self._id))

    def __getattr__(self, name):
        if name in ("delete", "merge", "merge_columns", "add_columns", "update_columns", "create",
                    "create_from_file"):
            raise unsupported(f"LanceFragment.{name}")
        raise AttributeError(name)


def _restore_fragment(uri: str, version: int, fragment_id: int) -> LanceFragment:
    from nanolance.lance.dataset import LanceDataset

    return LanceDataset(uri, version=version).get_fragment(fragment_id)


def __getattr__(name: str):
    """pylance names this module does not implement import as placeholders (see _stubs.py)."""
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.fragment.{name}")
    globals()[name] = value
    return value
