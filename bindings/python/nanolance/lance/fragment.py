# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.fragment`` as nanolance implements it: LanceFragment, its metadata records, and the writes
a distributed job makes fragment by fragment (``LanceFragment.create``, ``write_fragments``,
``delete``, ``merge_columns``, ``update_columns``) for ``LanceDataset.commit`` to publish."""

from __future__ import annotations

import json
import os
import warnings
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any, Callable, Dict, Iterable, Iterator, List, Optional, Tuple, Union

import pyarrow as pa
import pyarrow.dataset

from nanolance import _nanolance
from nanolance.lance._errors import native, unsupported

if TYPE_CHECKING:  # pragma: no cover
    from nanolance.lance.dataset import LanceDataset
    from nanolance.lance.schema import LanceSchema

DEFAULT_MAX_BYTES_PER_FILE = 90 * 1024 * 1024 * 1024


class _CallableStr(str):
    """``DataFile.path`` used to be a method: calling it still works, with a warning."""

    def __call__(self):
        warnings.warn("DataFile.path() is deprecated, use DataFile.path instead", DeprecationWarning)
        return self

    def __reduce__(self):
        return (str, (str(self),))


@dataclass
class DataFile:
    """A data file of a fragment: its path (relative to ``data/``), the field ids it holds and where."""

    _path: str
    fields: List[int]
    column_indices: List[int] = field(default_factory=list)
    file_major_version: int = 0
    file_minor_version: int = 0
    file_size_bytes: Optional[int] = None
    base_id: Optional[int] = None

    def __init__(self, path: str, fields: List[int], column_indices: Optional[List[int]] = None,
                 file_major_version: int = 0, file_minor_version: int = 0, file_size_bytes: Optional[int] = None,
                 base_id: Optional[int] = None):
        self._path = path
        self.fields = fields
        self.column_indices = column_indices or []
        self.file_major_version = file_major_version
        self.file_minor_version = file_minor_version
        self.file_size_bytes = file_size_bytes
        self.base_id = base_id

    def __repr__(self) -> str:
        return (f"DataFile(path='{self._path}', fields={self.fields}, column_indices={self.column_indices}, "
                f"file_major_version={self.file_major_version}, file_minor_version={self.file_minor_version}, "
                f"file_size_bytes={self.file_size_bytes})")

    @property
    def path(self) -> str:
        return _CallableStr(self._path)

    def field_ids(self) -> List[int]:
        warnings.warn("DataFile.field_ids is deprecated, use DataFile.fields instead", DeprecationWarning)
        return self.fields

    @classmethod
    def create(cls, dataset: "LanceDataset", path: str, *, base_id: Optional[int] = None) -> "DataFile":
        """The record of a Lance file already in the dataset's ``data/`` directory: its columns matched
        to the dataset's fields by name."""
        if base_id is not None:
            raise unsupported("DataFile.create with a base_id (multiple base paths)")
        from nanolance.lance.file import LanceFileReader

        full = os.path.join(dataset.uri, "data", path)
        if not os.path.exists(full):
            raise OSError(f"Not found: {full}")
        reader = LanceFileReader(full)
        meta = reader.metadata()
        schema = reader.metadata().schema
        fields, indices = [], []
        by_name = {f.name(): f for f in dataset.lance_schema.fields()}
        column = 0
        for arrow_field in schema:
            lance_field = by_name.get(arrow_field.name)
            if lance_field is None:
                raise ValueError(f"Field '{arrow_field.name}' not found in the dataset schema")
            for f in _pre_order(lance_field):
                fields.append(f.id())
                indices.append(column)
                column += 1
        major, minor = meta.major_version, meta.minor_version
        return cls(path, fields, indices, major, minor, os.path.getsize(full))


def _pre_order(f) -> List[Any]:
    out = [f]
    for c in f.children():
        out.extend(_pre_order(c))
    return out


class DeletionFile:
    """A fragment's deletion file: ``_deletions/{fragment}-{read_version}-{id}.{arrow|bin}``."""

    __slots__ = ("read_version", "id", "file_type", "num_deleted_rows", "base_id")

    def __init__(self, read_version: int, id: int, file_type: str, num_deleted_rows: int,
                 base_id: Optional[int] = None):
        if file_type not in ("array", "bitmap"):
            raise ValueError(f"Invalid deletion file type: {file_type}")
        self.read_version = int(read_version)
        self.id = int(id)
        self.file_type = file_type
        self.num_deleted_rows = int(num_deleted_rows)
        self.base_id = None if base_id is None else int(base_id)

    def path(self, fragment_id: int, base_uri: Optional[str] = None) -> str:
        suffix = "arrow" if self.file_type == "array" else "bin"
        name = f"_deletions/{fragment_id}-{self.read_version}-{self.id}.{suffix}"
        return name if base_uri is None else f"{base_uri.rstrip('/')}/{name}"

    def asdict(self) -> dict:
        return {"read_version": self.read_version, "id": self.id, "file_type": self.file_type,
                "num_deleted_rows": self.num_deleted_rows, "base_id": self.base_id}

    def json(self) -> str:
        return json.dumps(self.asdict())

    @staticmethod
    def from_json(data: str) -> "DeletionFile":
        return DeletionFile(**json.loads(data))

    def __eq__(self, other) -> bool:
        return isinstance(other, DeletionFile) and self.asdict() == other.asdict()

    def __hash__(self) -> int:
        return hash(tuple(self.asdict().values()))

    def __repr__(self) -> str:
        return f"DeletionFile(type='{self.file_type}', num_deleted_rows={self.num_deleted_rows})"

    def __reduce__(self):
        return (DeletionFile, (self.read_version, self.id, self.file_type, self.num_deleted_rows, self.base_id))


@dataclass
class FragmentMetadata:
    """A fragment as a manifest records it: its id, data files, row count and deletion file."""

    id: int
    files: List[DataFile]
    physical_rows: int
    deletion_file: Optional[DeletionFile] = None
    row_id_meta: Optional[Any] = None
    created_at_version_meta: Optional[Any] = None
    last_updated_at_version_meta: Optional[Any] = None
    overlays: List[Any] = field(default_factory=list)

    @property
    def num_deletions(self) -> int:
        return 0 if self.deletion_file is None else self.deletion_file.num_deleted_rows

    @property
    def num_rows(self) -> int:
        return self.physical_rows - self.num_deletions

    def data_files(self) -> List[DataFile]:
        warnings.warn("FragmentMetadata.data_files is deprecated. Use .files instead.", DeprecationWarning)
        return self.files

    def to_json(self) -> dict:
        def file_json(f: DataFile) -> dict:
            d = asdict(f)
            d["path"] = d.pop("_path")
            return d

        return dict(
            id=self.id,
            files=[file_json(f) for f in self.files],
            overlays=[],
            physical_rows=self.physical_rows,
            deletion_file=None if self.deletion_file is None else self.deletion_file.asdict(),
            row_id_meta=None,
            created_at_version_meta=None,
            last_updated_at_version_meta=None,
        )

    @staticmethod
    def from_json(json_data: str) -> "FragmentMetadata":
        data = json.loads(json_data)
        deletion = data.get("deletion_file")
        if deletion is not None:
            deletion = DeletionFile(**deletion)
        for key in ("row_id_meta", "created_at_version_meta", "last_updated_at_version_meta"):
            if data.get(key) is not None:
                raise unsupported("stable row ids")
        if data.get("overlays"):
            raise unsupported("data overlay files")
        return FragmentMetadata(id=data["id"], files=[DataFile(**f) for f in data["files"]],
                                physical_rows=data["physical_rows"], deletion_file=deletion)


class LanceFragment(pa.dataset.Fragment):
    """One fragment of a dataset version. Mirrors ``lance.LanceFragment``."""

    def __init__(self, dataset: "LanceDataset", fragment_id: Optional[int], *, fragment=None,
                 _info: Optional[dict] = None, **kwargs):
        if fragment_id is None:
            raise ValueError("Either fragment or fragment_id must be specified")
        self._ds = dataset
        self._id = int(fragment_id)
        if _info is None:
            _info = next((f for f in dataset._info["fragments"] if f["id"] == self._id), None)
            if _info is None:
                raise ValueError(f"Fragment id does not exist: {fragment_id}")
        self._info = _info

    # ── uncommitted writes ────────────────────────────────────────────────────────────────────────

    @staticmethod
    def create(dataset_uri: Union[str, Path], data, fragment_id: Optional[int] = None,
               schema: Optional[pa.Schema] = None, max_rows_per_group: Optional[int] = 1024, progress=None,
               mode: str = "append", *, data_storage_version: Optional[str] = None,
               use_legacy_format: Optional[bool] = None, storage_options: Optional[Dict[str, str]] = None,
               namespace_client=None, table_id=None, session=None) -> FragmentMetadata:
        """Write ``data`` as one fragment's data file, uncommitted: commit it with
        ``LanceOperation.Append`` or ``Overwrite``."""
        _check_namespace(namespace_client, table_id)
        data_storage_version = _storage_version(data_storage_version, use_legacy_format)
        fragments, _ = _write_uncommitted(data, dataset_uri, schema, mode=mode, max_rows_per_file=None,
                                          max_bytes_per_file=None, progress=progress,
                                          data_storage_version=data_storage_version, single=True)
        fragment = fragments[0]
        if fragment_id is not None:
            fragment.id = int(fragment_id)
        return fragment

    @staticmethod
    def create_from_file(filename: str, dataset: "LanceDataset", fragment_id: int) -> FragmentMetadata:
        """The fragment of a Lance file already in the dataset's ``data/`` directory."""
        f = DataFile.create(dataset, filename)
        from nanolance.lance.file import LanceFileReader

        rows = LanceFileReader(os.path.join(dataset.uri, "data", filename)).metadata().num_rows
        return FragmentMetadata(id=int(fragment_id), files=[f], physical_rows=int(rows))

    def delete(self, predicate: str) -> Optional[FragmentMetadata]:
        """The fragment with the rows where ``predicate`` holds deleted (a new deletion file; the data
        files are untouched), or None when no row would be left."""
        from nanolance.lance.dataset import _filter_sql

        with native():
            message = _nanolance._fragment_delete(self._ds._uri, self._ds.version, self._id, _filter_sql(predicate),
                                                  [])
        return None if message is None else _decode_fragment(message)

    def delete_rows(self, offsets: Iterable[int]) -> Optional[FragmentMetadata]:
        """The fragment with the rows at these physical offsets deleted, or None when none is left."""
        with native():
            message = _nanolance._fragment_delete(self._ds._uri, self._ds.version, self._id, None,
                                                  [int(o) for o in offsets])
        return None if message is None else _decode_fragment(message)

    def merge_columns(self, value_func, columns: Optional[List[str]] = None, batch_size: Optional[int] = None,
                      reader_schema: Optional[pa.Schema] = None) -> Tuple[FragmentMetadata, "LanceSchema"]:
        """New columns for this fragment, uncommitted: from SQL expressions (``{name: expr}``), a
        function of each batch of the fragment (``columns`` read), or data (one row per row).
        Commit the fragments with ``LanceOperation.Merge`` and the schema returned."""
        from nanolance.lance.udf import BatchUDF, normalize_transform

        transforms = normalize_transform(value_func, self, columns, reader_schema)
        if isinstance(transforms, BatchUDF) and transforms.cache is not None:
            raise ValueError("A checkpoint file cannot be used when applying a UDF with "
                             "LanceFragment.merge_columns.  You must apply your own checkpointing for "
                             "fragment-level operations.")
        if isinstance(transforms, dict):
            with native():
                message, fields = _nanolance._fragment_add_columns_sql(
                    self._ds._uri, self._ds.version, self._id, [(str(k), str(v)) for k, v in transforms.items()],
                    self._ds.max_field_id)
            from nanolance.lance.schema import LanceSchema

            base = self._ds.lance_schema
            return _decode_fragment(message), LanceSchema._from_messages(list(base._messages) + list(fields),
                                                                         base._metadata)
        table = self._new_columns(transforms, columns, batch_size)
        return self._write_columns(table, replace=False)

    def merge(self, data_obj, left_on: str, right_on: Optional[str] = None,
              schema=None) -> Tuple[FragmentMetadata, "LanceSchema"]:
        """This fragment left-joined with ``data_obj`` on ``left_on`` = ``right_on``: the right side's
        other columns become new columns (null where no row matches)."""
        from nanolance.lance.dataset import _coerce_reader_raw as _coerce_reader  # JSON as text, like a scan

        right_on = left_on if right_on is None else right_on
        right = _coerce_reader(data_obj, schema).read_all()
        if right_on not in right.schema.names:
            raise OSError(f"Invalid user input: Column {right_on} does not exist in the right side fragment")
        keys = self._left_keys(left_on)
        take = _match(keys, right.column(right_on))
        added = right.drop_columns([right_on]).take(take)
        return self._write_columns(added, replace=False)

    def update_columns(self, data_obj, left_on: str = "_rowid", right_on: Optional[str] = None, schema=None, *,
                       with_offsets: bool = False):
        """New values for existing columns, uncommitted: each row matching a row of ``data_obj`` on
        ``left_on`` = ``right_on`` takes its values; the rest keep theirs. Commit with
        ``LanceOperation.Update(updated_fragments=..., fields_modified=...)``."""
        from nanolance.lance.dataset import _coerce_reader_raw as _coerce_reader  # JSON as text, like a scan

        right_on = left_on if right_on is None else right_on
        names = self._ds._data_schema.names
        if left_on not in names and left_on not in ("_rowid", "_rowaddr"):
            raise ValueError(f"Invalid user input: Column {left_on} does not exist in the left side fragment")
        right = _coerce_reader(data_obj, schema).read_all()
        if right_on not in right.schema.names:
            raise ValueError(f"Invalid user input: Column {right_on} does not exist in the right side fragment")
        write = [n for n in right.schema.names if n != right_on]
        for name in write:
            if name in ("_rowid", "_rowaddr"):
                raise ValueError(f"Invalid user input: Column {name} is a reversed metadata column and cannot "
                                 "be updated")
            if name not in names:
                raise ValueError(f"Invalid user input: Column {name} in right side fragment does not exist in "
                                 "left side fragment")
        current = self._live_table(write + ([left_on] if left_on not in ("_rowid", "_rowaddr") else []),
                                   with_row_id=left_on == "_rowid", with_row_address=left_on == "_rowaddr")
        from nanolance.lance.dataset import _holds_blob

        target = self._ds._data_schema
        for name in write:
            if _holds_blob(target.field(name).type):
                raise unsupported("LanceFragment.update_columns of a blob column")
        rows = _match(current.column(left_on), right.column(right_on)).to_pylist()
        matched = [i for i, t in enumerate(rows) if t is not None]
        columns = []
        for name in write:
            # Each row's value: the old one, or the matching row's -- taken from both, one after the other.
            old = current.column(name).combine_chunks()
            new = right.column(name).combine_chunks()
            if new.type != old.type:
                new = new.cast(old.type)
            index = pa.array([len(old) + t if t is not None else i for i, t in enumerate(rows)], pa.int64())
            columns.append(pa.concat_arrays([old, new]).take(index))
        table = pa.table(columns, schema=pa.schema([current.schema.field(n) for n in write]))
        metadata, fields_modified = self._write_columns(table, replace=True)
        if with_offsets:
            live = self._live_offsets()
            from nanolance.lance.bitmap import Bitmap

            return metadata, fields_modified, Bitmap([live[i] for i in matched]).serialize()
        return metadata, fields_modified

    def validate(self) -> None:
        """Check the fragment's files agree with each other and with the manifest."""
        seen = set()
        for f in self.metadata.files:
            if len(f.fields) != len(f.column_indices):
                raise OSError(f"Corrupt fragment {self._id}: data file {f.path} has {len(f.fields)} fields and "
                              f"{len(f.column_indices)} column indices")
            for field_id in f.fields:
                if field_id >= 0 and field_id in seen:
                    raise OSError(f"Corrupt fragment {self._id}: field {field_id} is in more than one data file")
                seen.add(field_id)
        self.count_rows()

    # ── helpers for the writes above ──────────────────────────────────────────────────────────────

    def _live_table(self, columns, with_row_id=False, with_row_address=False, batch_size=None) -> pa.Table:
        return self.scanner(columns=columns, with_row_id=with_row_id, with_row_address=with_row_address,
                            batch_size=batch_size).to_table()

    def _live_offsets(self) -> List[int]:
        addresses = self.scanner(columns=[], with_row_address=True).to_table().column("_rowaddr").to_pylist()
        return [a & 0xFFFFFFFF for a in addresses]

    def _left_keys(self, left_on: str) -> pa.ChunkedArray:
        if left_on in ("_rowid", "_rowaddr"):
            table = self._live_table([], with_row_id=left_on == "_rowid", with_row_address=left_on == "_rowaddr")
        else:
            if left_on not in self._ds._data_schema.names:
                raise OSError(f"Invalid user input: Column {left_on} does not exist in the left side fragment")
            table = self._live_table([left_on])
        return table.column(left_on)

    def _new_columns(self, transforms, columns, batch_size, cache=None) -> pa.Table:
        """The new columns' values for the fragment's live rows, in order. With `cache` (a UDF's
        checkpoint), a batch computed before is taken from it, and each one computed is kept there."""
        from nanolance.lance.udf import BatchUDF

        if isinstance(transforms, dict):
            raise unsupported("LanceFragment.merge_columns with SQL expressions")
        if isinstance(transforms, BatchUDF):
            read = None
            with_row_id = with_row_address = False
            if columns is not None:
                read = [c for c in columns if c not in ("_rowid", "_rowaddr")]
                with_row_id = "_rowid" in columns
                with_row_address = "_rowaddr" in columns
            batches = []
            index = -1
            for batch in self.scanner(columns=read, with_row_id=with_row_id, with_row_address=with_row_address,
                                      batch_size=batch_size).to_batches():
                if batch.num_rows == 0:
                    continue
                index += 1
                info = None if cache is None else cache.BatchInfo(self._id, index)
                out = None if cache is None else cache.get_batch(info)
                if out is None:
                    out = transforms._call(batch)
                    if cache is not None:
                        cache.insert_batch(info, out)
                if out.num_rows != batch.num_rows:
                    raise ValueError(f"The UDF returned {out.num_rows} rows for a batch of {batch.num_rows}")
                batches.append(out)
            return pa.Table.from_batches(batches, schema=transforms.output_schema)
        reader = transforms
        return pa.Table.from_batches(list(reader), schema=reader.schema)

    def _write_columns(self, live: pa.Table, replace: bool):
        """Write `live` (a value per live row) as a new data file of this fragment: deleted rows take
        a copy of a live row, so a column stays as non-nullable as its values."""
        if live.num_rows != self.count_rows():
            raise ValueError(f"Expected {self.count_rows()} rows for fragment {self._id}, got {live.num_rows}")
        physical = self.physical_rows
        if physical != live.num_rows:
            offsets = self._live_offsets()
            take, at = [], 0
            for row in range(physical):
                if at < len(offsets) and offsets[at] == row:
                    take.append(at)
                    at += 1
                else:
                    take.append(min(at, len(offsets) - 1))
            live = live.take(pa.array(take, pa.int64()))
        from nanolance.lance.dataset import _coerce_reader

        reader = _coerce_reader(live.combine_chunks())  # JSON columns as the JSONB Lance stores
        with native():
            message, fields, written = _nanolance._fragment_write_columns(
                self._ds._uri, self._ds.version, self._id, reader, replace, self._ds.max_field_id)
        metadata = _decode_fragment(message)
        if replace:
            return metadata, [int(i) for i in written]
        from nanolance.lance.schema import LanceSchema

        base = self._ds.lance_schema
        return metadata, LanceSchema._from_messages(list(base._messages) + list(fields), base._metadata)

    # ── reads ─────────────────────────────────────────────────────────────────────────────────────

    @property
    def fragment_id(self) -> int:
        return self._id

    @property
    def metadata(self) -> FragmentMetadata:
        messages = self._ds._fragment_messages()
        return _decode_fragment(messages[self._id])

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
        return self._ds._data_schema

    def data_files(self) -> List[DataFile]:
        return self.metadata.files

    def deletion_file(self) -> Optional[str]:
        return self._info["deletion_file"] or None

    @property
    def files(self) -> List[DataFile]:
        return self.metadata.files

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

    def to_pandas(self, columns=None, filter=None, limit=None, offset=None, batch_size=None, with_row_id=False,
                  with_row_address=False, blob_mode: str = "lazy", order_by=None, **kwargs):
        """This fragment as a pandas DataFrame; ``kwargs`` go to ``pyarrow.Table.to_pandas``."""
        return self.scanner(columns=columns, filter=filter, limit=limit, offset=offset, batch_size=batch_size,
                            with_row_id=with_row_id, with_row_address=with_row_address,
                            order_by=order_by).to_pandas(blob_mode=blob_mode, **kwargs)

    def to_batches(self, columns=None, batch_size=None, filter=None, limit=None, offset=None, with_row_id=False,
                   **kwargs) -> Iterator[pa.RecordBatch]:
        return self.scanner(columns=columns, batch_size=batch_size, filter=filter, limit=limit, offset=offset,
                            with_row_id=with_row_id, **kwargs).to_batches()

    def head(self, num_rows: int) -> pa.Table:
        return self.to_table(limit=num_rows)

    # pyarrow.dataset.Fragment's other members, overridden (see LanceDataset).
    @property
    def physical_schema(self) -> pa.Schema:
        raise NotImplementedError("Not implemented yet for LanceFragment")

    @property
    def partition_expression(self):
        raise NotImplementedError("Not implemented yet for LanceFragment")

    def take(self, indices, columns=None) -> pa.Table:
        addresses = [(self._id << 32) | int(i) for i in indices]
        return self._ds._take_rows(addresses, columns=columns)

    def __repr__(self) -> str:
        return f"LanceFragment(id={self._id})"

    def __reduce__(self):
        return (_restore_fragment, (self._ds.uri, self._ds.version, self._id,
                                    getattr(self._ds, "_storage_options", None)))


def _restore_fragment(uri: str, version: int, fragment_id: int, storage_options=None) -> LanceFragment:
    from nanolance.lance.dataset import LanceDataset

    return LanceDataset(uri, version=version, storage_options=storage_options).get_fragment(fragment_id)


def _decode_fragment(message: bytes) -> FragmentMetadata:
    from nanolance.lance._transactions import _fragment

    return _fragment(bytes(message))


def _match(left: Union[pa.Array, pa.ChunkedArray], right: Union[pa.Array, pa.ChunkedArray]) -> pa.Array:
    """For each left key, the row of the right side with that key (its last), or null."""
    index: Dict[Any, int] = {}
    for row, key in enumerate(right.to_pylist()):
        if key is not None:
            index[key] = row
    return pa.array([None if k is None else index.get(k) for k in left.to_pylist()], pa.int64())


def _check_namespace(namespace_client, table_id) -> None:
    if (namespace_client is None) != (table_id is None):
        raise ValueError("Both 'namespace_client' and 'table_id' must be provided together.")
    if namespace_client is not None:
        raise unsupported("namespaces")


def _storage_version(data_storage_version: Optional[str], use_legacy_format: Optional[bool]) -> Optional[str]:
    if use_legacy_format is not None:
        warnings.warn("use_legacy_format is deprecated, use data_storage_version instead", DeprecationWarning)
        data_storage_version = "legacy" if use_legacy_format else "stable"
    return data_storage_version


def _write_uncommitted(data, dataset_uri, schema, *, mode: str, max_rows_per_file: Optional[int],
                       max_bytes_per_file: Optional[int], progress, data_storage_version: Optional[str],
                       single: bool = False, external_blob_mode: str = "reference",
                       allow_external_blob_outside_bases: bool = False):
    """Write `data` as data files under `dataset_uri` without committing them: the fragments (ids 0,
    to be assigned at commit) and the schema they were written with (a LanceSchema)."""
    from nanolance.lance.dataset import LanceDataset, _coerce_reader, _path_of, _stage_write
    from nanolance.lance.schema import LanceSchema

    if mode not in ("append", "create", "overwrite"):
        raise ValueError(f"Invalid mode: {mode}")
    if data_storage_version not in (None, "stable", "2.2", "next"):
        raise unsupported(f"data_storage_version={data_storage_version!r} (nanolance writes 2.2)")
    path = _path_of(dataset_uri if not isinstance(dataset_uri, Path) else str(dataset_uri))
    reader = _coerce_reader(data, schema)
    if single and (len(reader.schema) == 0):
        raise OSError("Invalid user input: Cannot write a fragment without any columns")
    writer, wrote_rows = _stage_write(path, reader, mode="append" if mode == "append" else "overwrite",
                                      max_rows_per_file=max_rows_per_file, max_bytes_per_file=max_bytes_per_file,
                                      external_blob_mode=external_blob_mode,
                                      allow_external_blob_outside_bases=allow_external_blob_outside_bases,
                                      keep_empty=False)
    with native():
        messages, fields = writer.take(False)
    fragments = [_decode_fragment(m) for m in messages]
    if single:
        if wrote_rows == 0 or not fragments:
            raise OSError("Invalid user input: Cannot write a fragment with no rows")
        if len(fragments) > 1:  # one fragment, however many files the writer cut
            raise OSError("LanceFragment.create wrote more than one data file")
    if progress is not None:
        for fragment in fragments:
            progress.begin(fragment)
            progress.complete(fragment)
    lance_schema = LanceSchema._from_messages(list(fields), {k.decode(): v.decode()
                                                             for k, v in (reader.schema.metadata or {}).items()})
    return fragments, lance_schema


def write_fragments(data, dataset_uri, schema: Optional[pa.Schema] = None, *, return_transaction: bool = False,
                    mode: str = "append", max_rows_per_file: int = 1024 * 1024,
                    max_rows_per_group: Optional[int] = 1024, max_bytes_per_file: int = DEFAULT_MAX_BYTES_PER_FILE,
                    progress=None, data_storage_version: Optional[str] = None,
                    use_legacy_format: Optional[bool] = None, storage_options: Optional[Dict[str, str]] = None,
                    enable_stable_row_ids: bool = False, target_bases=None, target_all_bases=None,
                    initial_bases=None, base_store_params=None, external_blob_mode: str = "reference",
                    allow_external_blob_outside_bases: bool = False, namespace_client=None, table_id=None,
                    session=None):
    """Write ``data`` into one or more fragments, uncommitted (a distributed write's per-worker step):
    their FragmentMetadata, or with ``return_transaction`` the Transaction that commits them."""
    from nanolance.lance._transactions import LanceOperation, Transaction
    from nanolance.lance.dataset import LanceDataset, _exists, _path_of

    _check_namespace(namespace_client, table_id)
    if enable_stable_row_ids:
        raise unsupported("stable row ids")
    if target_bases or target_all_bases or initial_bases:
        raise unsupported("multiple base paths")
    if external_blob_mode not in ("reference", "ingest"):
        raise ValueError(f"Invalid user input: Invalid external blob mode: {external_blob_mode}")
    if external_blob_mode == "ingest" and allow_external_blob_outside_bases:
        raise OSError('Invalid user input: allow_external_blob_outside_bases only applies when '
                      'external_blob_mode="reference"')
    if not isinstance(dataset_uri, (str, Path, LanceDataset)):
        raise TypeError(f"Unknown dataset_uri type {type(dataset_uri)}")
    data_storage_version = _storage_version(data_storage_version, use_legacy_format)
    fragments, lance_schema = _write_uncommitted(
        data, dataset_uri, schema, mode=mode, max_rows_per_file=max_rows_per_file,
        max_bytes_per_file=max_bytes_per_file, progress=progress, data_storage_version=data_storage_version,
        external_blob_mode=external_blob_mode, allow_external_blob_outside_bases=allow_external_blob_outside_bases)
    if not return_transaction:
        return fragments
    path = _path_of(dataset_uri)
    read_version = LanceDataset(path).latest_version if _exists(path) else 0
    if mode == "append":
        operation = LanceOperation.Append(fragments)
    else:
        operation = LanceOperation.Overwrite(lance_schema, fragments)
    return Transaction(read_version, operation)


def __getattr__(name: str):
    """pylance names this module does not implement import as placeholders (see _stubs.py)."""
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.fragment.{name}")
    globals()[name] = value
    return value
