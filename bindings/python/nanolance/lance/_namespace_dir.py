# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Lance's DirectoryNamespace (lance-namespace-impls 12, ``dir.rs`` and ``dir/manifest.rs``) over
nanolance's datasets.

A namespace is a directory. Root tables live at ``<root>/<name>.lance``; with the manifest enabled
(the default) the ``<root>/__manifest`` Lance table also records every namespace and table:
``object_id`` (the id's parts joined by ``$``), ``object_type`` (``namespace`` / ``table``),
``location`` (relative to the root), ``metadata`` (the properties as JSON) and ``base_objects``.
Every change rewrites that table as one fragment and commits it as an Overwrite at exactly the next
version -- the put-if-not-exists Lance's rewrite relies on, so concurrent writers, in this process
or another, retry against what the winner committed. Tables of child namespaces get
``<hash>_<object_id>`` directories, as Lance names them; either library reads the other's catalog.

Requests arrive as dicts (pydantic ``model_dump()``s) and responses leave as dicts for the
``from_dict`` of lance_namespace's models: the interface of pylance's native ``PyDirectoryNamespace``.
Errors are lance_namespace's exceptions, worded as Lance words them.
"""

from __future__ import annotations

import io
import json
import os
import random
import shutil
import threading
import time
import uuid
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import quote

import pyarrow as pa

_MANIFEST = "__manifest"
_DELIMITER = "$"
_PK_POSITION = "lance-schema:unenforced-primary-key:position"
_READER_FLAGS = "lance.namespace.manifest.reader_feature_flags"
_WRITER_FLAGS = "lance.namespace.manifest.writer_feature_flags"
_DEFAULT_REWRITE_RETRIES = 20
_U64_MAX = (1 << 64) - 1

_MANIFEST_SCHEMA = pa.schema([
    pa.field("object_id", pa.utf8(), False, metadata={_PK_POSITION: "0"}),
    pa.field("object_type", pa.utf8(), False),
    pa.field("location", pa.utf8(), True),
    pa.field("metadata", pa.utf8(), True),
    pa.field("base_objects", pa.list_(pa.field("object_id", pa.utf8(), True)), True),
])

# lance_namespace's exception classes and the prefix NamespaceError's Display gives each message.
_KINDS = {
    "Unsupported": ("UnsupportedOperationError", "Unsupported"),
    "NamespaceNotFound": ("NamespaceNotFoundError", "Namespace not found"),
    "NamespaceAlreadyExists": ("NamespaceAlreadyExistsError", "Namespace already exists"),
    "NamespaceNotEmpty": ("NamespaceNotEmptyError", "Namespace not empty"),
    "TableNotFound": ("TableNotFoundError", "Table not found"),
    "TableAlreadyExists": ("TableAlreadyExistsError", "Table already exists"),
    "TableIndexNotFound": ("TableIndexNotFoundError", "Table index not found"),
    "TableIndexAlreadyExists": ("TableIndexAlreadyExistsError", "Table index already exists"),
    "TableTagNotFound": ("TableTagNotFoundError", "Table tag not found"),
    "TableTagAlreadyExists": ("TableTagAlreadyExistsError", "Table tag already exists"),
    "TransactionNotFound": ("TransactionNotFoundError", "Transaction not found"),
    "TableVersionNotFound": ("TableVersionNotFoundError", "Table version not found"),
    "TableColumnNotFound": ("TableColumnNotFoundError", "Table column not found"),
    "InvalidInput": ("InvalidInputError", "Invalid input"),
    "ConcurrentModification": ("ConcurrentModificationError", "Concurrent modification"),
    "Internal": ("InternalError", "Internal error"),
    "InvalidTableState": ("InvalidTableStateError", "Invalid table state"),
    "TableBranchNotFound": ("TableBranchNotFoundError", "Table branch not found"),
    "TableBranchAlreadyExists": ("TableBranchAlreadyExistsError", "Table branch already exists"),
    "PermissionDenied": ("PermissionDeniedError", "Permission denied"),
    "Unauthenticated": ("UnauthenticatedError", "Unauthenticated"),
    "ServiceUnavailable": ("ServiceUnavailableError", "Service unavailable"),
    "TableSchemaValidationError": ("TableSchemaValidationError", "Table schema validation error"),
    "Throttling": ("ThrottlingError", "Throttling"),
}


def ns_error(kind: str, message: str) -> Exception:
    """lance_namespace's exception for a NamespaceError of `kind`: its text is the error's Display
    (``"Table not found: ..."``); ``ns_message`` keeps the bare message, which the REST adapter sends."""
    from lance_namespace import errors

    name, prefix = _KINDS[kind]
    exc = getattr(errors, name)(f"{prefix}: {message}")
    exc.ns_message = message
    return exc


def _is_ns_error(exc: BaseException) -> bool:
    from lance_namespace.errors import LanceNamespaceError

    return isinstance(exc, LanceNamespaceError)


def _str_to_bool(value: Optional[str]) -> Optional[bool]:
    if value is None:
        return None
    text = str(value).lower()
    if text in ("1", "true", "on", "yes", "y"):
        return True
    if text in ("0", "false", "off", "no", "n"):
        return False
    return None


def _as_dict(request) -> Dict[str, Any]:
    if request is None:
        return {}
    if hasattr(request, "model_dump"):
        return request.model_dump()
    return dict(request)


def _paginate(names: List[str], page_token: Optional[str], limit: Optional[int]) -> Optional[str]:
    """Sort, skip to after ``page_token``, keep ``limit``: Lance's apply_pagination."""
    names.sort()
    if page_token is not None:
        start = next((i for i, n in enumerate(names) if n > page_token), None)
        names[:] = names[start:] if start is not None else []
    if limit is not None and limit >= 0 and len(names) > limit:
        token = names[limit - 1] if limit > 0 else None
        del names[limit:]
        return token
    return None


def _object_id(parts: List[str]) -> str:
    return _DELIMITER.join(parts)


def _split_id(parts: List[str]) -> Tuple[List[str], str]:
    return list(parts[:-1]), parts[-1]


def _format_table_id(parts: List[str]) -> str:
    return f"table id '{_object_id(parts)}'"


def _generate_dir_name(object_id: str) -> str:
    return f"{random.getrandbits(32):08x}_{object_id}"


def _root_url(root: str) -> str:
    """The root as a URL with a trailing slash, as lance_io's uri_to_url gives it."""
    if "://" in root:
        url = root
    else:
        path = os.path.abspath(os.path.expanduser(root))
        url = "file://" + quote(path, safe="/$!&'()*+,;=:@~-._")
    return url if url.endswith("/") else url + "/"


def construct_full_uri(root: str, relative_location: str) -> str:
    """A location relative to the root as a full URI (Lance's construct_full_uri)."""
    segments = [quote(s, safe="$!&'()*+,;=:@~-._") for s in relative_location.split("/") if s]
    return _root_url(root) + "/".join(segments)


def _manifest_version_from_filename(name: str) -> Optional[int]:
    """The version a ``_versions/`` file name records, or None (V2: inverted, V1: plain)."""
    if not name.endswith(".manifest"):
        return None
    stem = name[: -len(".manifest")]
    if not stem.isdigit():
        return None
    if len(stem) == 20:
        return _U64_MAX - int(stem)
    return int(stem)


def _store_path(path: str) -> str:
    """An object-store path for a local file: no leading slash, '/' separators."""
    return os.path.abspath(path).replace(os.sep, "/").lstrip("/")


def _put_marker_atomic(path: str) -> bool:
    """Create ``path`` holding b"reserved" unless it exists (put-if-not-exists); False if it did."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    except FileExistsError:
        return False
    with os.fdopen(fd, "wb") as f:
        f.write(b"reserved")
    return True


def _ipc_table(data: bytes, operation: str) -> pa.Table:
    if not data:
        raise ns_error("InvalidInput", f"Request data (Arrow IPC stream) is required for {operation}")
    try:
        reader = pa.ipc.open_stream(pa.BufferReader(data))
    except Exception as exc:  # noqa: BLE001
        raise ns_error("InvalidInput", f"Invalid Arrow IPC stream: {exc}") from None
    try:
        return reader.read_all()
    except Exception as exc:  # noqa: BLE001
        raise ns_error("Internal", f"Failed to read batch from IPC stream: {exc}") from None


def _metadata_json(properties: Optional[Dict[str, str]]) -> Optional[str]:
    if not properties:
        return None
    return json.dumps(properties, separators=(",", ":"))


def _parse_metadata(text: Optional[str], kind: str, object_id: str) -> Optional[Dict[str, str]]:
    if text is None:
        return None
    try:
        value = json.loads(text)
        if not isinstance(value, dict):
            raise ValueError("not a map")
        return {str(k): str(v) for k, v in value.items()}
    except Exception as exc:  # noqa: BLE001
        raise ns_error("Internal", f"Failed to deserialize metadata for {kind} '{object_id}': {exc}") from None


def _has_manifests(path: str) -> bool:
    versions = os.path.join(path, "_versions")
    try:
        return any(True for _ in os.scandir(versions))
    except FileNotFoundError:
        return False


class _Row:
    __slots__ = ("object_id", "object_type", "location", "metadata", "base_objects")

    def __init__(self, object_id, object_type, location=None, metadata=None, base_objects=None):
        self.object_id = object_id
        self.object_type = object_type
        self.location = location
        self.metadata = metadata
        self.base_objects = base_objects


class _Retry(Exception):
    """A rewrite lost the race for the next version; try again against what won."""


class _ManifestNamespace:
    """The ``__manifest`` table: Lance's ManifestNamespace."""

    def __init__(self, owner: "PyDirectoryNamespace"):
        self.owner = owner
        self.path = os.path.join(owner._base, _MANIFEST)
        self._lock = threading.Lock()

    # ── reading ───────────────────────────────────────────────────────────────────────────────────

    def _dataset(self):
        from nanolance.lance.dataset import LanceDataset

        return LanceDataset(self.path)

    @staticmethod
    def _flags(ds, key: str) -> int:
        raw = (ds.metadata or {}).get(key) if hasattr(ds, "metadata") else None
        if raw is None:
            return 0
        try:
            return int(raw)
        except ValueError:
            raise ns_error("Unsupported", (
                f"The __manifest dataset has an unparsable feature-flag value '{raw}' for '{key}': invalid "
                "digit found in string. This likely means it was written by a newer, incompatible version "
                "of Lance; please upgrade Lance to use this catalog.")) from None

    def _ensure_readable(self, ds) -> None:
        flags = self._flags(ds, _READER_FLAGS)
        if flags:
            raise ns_error("Unsupported", (
                f"The __manifest dataset was written with reader feature flags {flags}, which this version "
                "of Lance does not understand (known reader flags: 0). Please upgrade Lance to read this "
                "catalog."))

    def _ensure_writable(self, ds) -> None:
        flags = self._flags(ds, _WRITER_FLAGS)
        if flags:
            raise ns_error("Unsupported", (
                f"The __manifest dataset was written with writer feature flags {flags}, which this version "
                "of Lance does not understand (known writer flags: 0). Please upgrade Lance to modify this "
                "catalog."))

    def rows(self, ds=None) -> List[_Row]:
        ds = ds if ds is not None else self._dataset()
        table = ds.to_table(columns=["object_id", "object_type", "location", "metadata", "base_objects"])
        cols = [table.column(i).to_pylist() for i in range(5)]
        return [_Row(*values) for values in zip(*cols)]

    def contains(self, object_id: str) -> bool:
        return any(r.object_id == object_id for r in self.rows())

    def table_info(self, object_id: str) -> Optional[_Row]:
        found = [r for r in self.rows() if r.object_id == object_id and r.object_type == "table"]
        if len(found) > 1:
            raise ns_error("Internal", f"Expected exactly 1 table with id '{object_id}', found {len(found)}")
        return found[0] if found else None

    def namespace_info(self, object_id: str) -> Optional[_Row]:
        found = [r for r in self.rows() if r.object_id == object_id and r.object_type == "namespace"]
        if len(found) > 1:
            raise ns_error("Internal", f"Expected exactly 1 namespace with id '{object_id}', found {len(found)}")
        return found[0] if found else None

    def table_locations(self) -> set:
        return {r.location for r in self.rows() if r.object_type == "table" and _DELIMITER not in r.object_id}

    def _children(self, rows: List[_Row], parent: List[str], kind: str) -> List[_Row]:
        if not parent:
            return [r for r in rows if r.object_type == kind and _DELIMITER not in r.object_id]
        prefix = _object_id(parent) + _DELIMITER
        return [r for r in rows if r.object_type == kind and r.object_id.startswith(prefix)
                and _DELIMITER not in r.object_id[len(prefix):]]

    def location_has_manifests(self, location: str) -> bool:
        return _has_manifests(os.path.join(self.owner._base, location))

    # ── writing ───────────────────────────────────────────────────────────────────────────────────

    def ensure_up_to_date(self) -> None:
        """Create the table if it is missing (an empty first version), or add the primary-key field
        metadata an older one lacks: Lance's ensure_manifest_table_up_to_date."""
        from nanolance.lance.dataset import LanceDataset, _exists, write_dataset

        if _exists(self.path):
            ds = LanceDataset(self.path)
            self._ensure_readable(ds)
            field = ds.schema.field("object_id") if "object_id" in ds.schema.names else None
            if field is not None and _PK_POSITION.encode() not in (field.metadata or {}):
                self._ensure_writable(ds)
                ds.update_field_metadata({"object_id": {_PK_POSITION: "0"}})
            return
        try:
            write_dataset(_MANIFEST_SCHEMA.empty_table(), self.path, mode="create")
        except Exception:  # noqa: BLE001 -- another writer created it first
            if not _exists(self.path):
                raise

    def ensure_writable(self) -> None:
        self._ensure_writable(self._dataset())

    def _commit_rows(self, ds, rows: List[_Row]) -> None:
        """Rewrite the table as `rows`, committed as an Overwrite at exactly ds.version + 1."""
        from nanolance.lance.commit import CommitConflictError
        from nanolance.lance.dataset import LanceDataset
        from nanolance.lance.fragment import write_fragments

        table = pa.table([
            pa.array([r.object_id for r in rows], pa.utf8()),
            pa.array([r.object_type for r in rows], pa.utf8()),
            pa.array([r.location for r in rows], pa.utf8()),
            pa.array([r.metadata for r in rows], pa.utf8()),
            pa.array([r.base_objects for r in rows], _MANIFEST_SCHEMA.field("base_objects").type),
        ], schema=_MANIFEST_SCHEMA)
        txn = write_fragments(table, self.path, mode="overwrite", return_transaction=True,
                              max_rows_per_file=(1 << 32) - 1, max_bytes_per_file=(1 << 62))
        try:
            LanceDataset.commit(self.path, txn.operation, read_version=ds.version, max_retries=0)
        except (CommitConflictError, OSError) as exc:
            if isinstance(exc, OSError) and "conflict" not in str(exc).lower():
                raise
            for fragment in txn.operation.fragments:
                for data_file in fragment.files:
                    try:
                        os.remove(os.path.join(self.path, "data", data_file.path))
                    except OSError:
                        pass
            raise _Retry() from None

    def rewrite(self, operation: str, mutate, conflict=None):
        """Apply ``mutate(rows) -> (new_rows or None, result)`` to the latest rows and commit; on a
        lost race, ``conflict()`` decides (None: retry, else a result to return) as Lance's
        ConflictResolution does."""
        max_retries = self.owner._commit_retries
        if max_retries is None:
            max_retries = _DEFAULT_REWRITE_RETRIES
        retries = 0
        with self._lock:
            while True:
                ds = self._dataset()
                self._ensure_writable(ds)
                new_rows, result = mutate(self.rows(ds))
                if new_rows is None:
                    return result
                try:
                    self._commit_rows(ds, new_rows)
                    return result
                except _Retry:
                    if conflict is not None:
                        outcome = conflict()
                        if outcome is not None:
                            return outcome[0]
                    if retries >= max_retries:
                        raise ns_error("ConcurrentModification",
                                       f"{operation}: still conflicting after {max_retries} retries") from None
                    retries += 1
                    time.sleep(0.01 * retries)

    def insert(self, entries: List[_Row], upsert: bool = False) -> None:
        ids = {e.object_id for e in entries}

        def mutate(rows):
            out, matched = [], set()
            for r in rows:
                if r.object_id in ids:
                    if not upsert:
                        raise ns_error("ConcurrentModification",
                                       f"Object '{r.object_id}' was concurrently created by another operation")
                    matched.add(r.object_id)
                    out.append(next(e for e in entries if e.object_id == r.object_id))
                else:
                    out.append(r)
            out.extend(e for e in entries if e.object_id not in matched)
            return out, None

        def conflict():
            if not upsert:
                for object_id in ids:
                    if self.contains(object_id):
                        raise ns_error("ConcurrentModification",
                                       f"Object '{object_id}' was concurrently created by another operation")
            return None

        self.rewrite("Failed to overwrite manifest", mutate, conflict)

    def delete(self, object_id: str) -> None:
        def mutate(rows):
            out = [r for r in rows if r.object_id != object_id]
            return (out if len(out) != len(rows) else None), None

        def conflict():
            return None if self.contains(object_id) else (None,)

        self.rewrite("Failed to delete from manifest", mutate, conflict)

    def validate_namespace_levels(self, namespace: List[str]) -> None:
        rows = self.rows()
        ids = {r.object_id for r in rows}
        for i in range(1, len(namespace) + 1):
            object_id = _object_id(namespace[:i])
            if object_id not in ids:
                raise ns_error("NamespaceNotFound", f"parent namespace '{object_id}'")

    # ── LanceNamespace operations ─────────────────────────────────────────────────────────────────

    def list_tables(self, req: Dict[str, Any]) -> Dict[str, Any]:
        parent = req.get("id")
        if parent is None:
            raise ns_error("InvalidInput", "Namespace ID is required")
        entries = [(r.object_id.split(_DELIMITER)[-1], r.location) for r in self._children(self.rows(), parent, "table")]
        if req.get("include_declared") is False:
            names = [name for name, location in entries if self.location_has_manifests(location)]
        else:
            names = [name for name, _ in entries]
        token = _paginate(names, req.get("page_token"), req.get("limit"))
        return {"tables": names, "page_token": token}

    def describe_table(self, req: Dict[str, Any]) -> Dict[str, Any]:
        table_id = req.get("id")
        if table_id is None:
            raise ns_error("InvalidInput", "Table ID is required")
        if not table_id:
            raise ns_error("InvalidInput", "Table ID cannot be empty")
        object_id = _object_id(table_id)
        info = self.table_info(object_id)
        if info is None:
            raise ns_error("TableNotFound", _format_table_id(table_id))
        namespace, name = _split_id(table_id)
        table_uri = construct_full_uri(self.owner._root, info.location)
        detailed = bool(req.get("load_detailed_metadata"))
        check_declared = detailed or bool(req.get("check_declared"))
        vend = req.get("vend_credentials")
        storage_options = self.owner._storage_options if vend is None or vend else None
        is_only_declared = (not self.location_has_manifests(info.location)) if check_declared else None
        properties = _parse_metadata(info.metadata, "table", object_id)
        out = {"table": name, "namespace": namespace, "location": table_uri, "table_uri": table_uri,
               "storage_options": storage_options, "properties": properties, "is_only_declared": is_only_declared}
        if not detailed or is_only_declared:
            return out
        try:
            ds = self.owner._open(os.path.join(self.owner._base, info.location), req.get("version"))
        except Exception as exc:  # noqa: BLE001
            raise ns_error("Internal", f"Table exists in manifest but failed to load dataset '{object_id}': {exc}") \
                from None
        out.update(version=ds.version, schema=_json_schema(ds.schema))
        return out

    def table_exists(self, req: Dict[str, Any]) -> None:
        table_id = req.get("id")
        if table_id is None:
            raise ns_error("InvalidInput", "Table ID is required")
        if not table_id:
            raise ns_error("InvalidInput", "Table ID cannot be empty")
        if not self.contains(_object_id(table_id)):
            raise ns_error("TableNotFound", _format_table_id(table_id))

    def create_table(self, req: Dict[str, Any], data: bytes) -> Dict[str, Any]:
        from nanolance.lance.dataset import write_dataset

        table_id = req.get("id")
        if table_id is None:
            raise ns_error("InvalidInput", "Table ID is required")
        if not table_id:
            raise ns_error("InvalidInput", "Table ID cannot be empty")
        namespace, name = _split_id(table_id)
        object_id = _object_id(table_id)
        self.ensure_writable()
        existing = self.table_info(object_id)
        existing_has_manifests = self.location_has_manifests(existing.location) if existing else None
        properties = req.get("properties")
        if existing_has_manifests is False and properties:
            raise ns_error("InvalidInput",
                           f"create_table cannot set properties for already declared table '{object_id}'")
        mode = "create" if existing_has_manifests is False else _create_mode(req.get("mode"))
        if existing is not None:
            dir_name = existing.location
        elif not namespace and self.owner._dir_listing_enabled:
            dir_name = f"{name}.lance"
        else:
            dir_name = _generate_dir_name(object_id)
        table_uri = construct_full_uri(self.owner._root, dir_name)
        overwriting = existing_has_manifests is True and mode == "overwrite"
        if existing_has_manifests is True:
            if mode == "create":
                raise ns_error("TableAlreadyExists", name)
            if mode == "exist_ok":
                return {"location": table_uri, "storage_options": self.owner._storage_options,
                        "properties": _parse_metadata(existing.metadata, "table", object_id)}
        if not data:
            raise ns_error("InvalidInput", "Request data (Arrow IPC stream) is required for create_table")
        try:
            table = pa.ipc.open_stream(pa.BufferReader(data)).read_all()
        except Exception as exc:  # noqa: BLE001
            raise ns_error("Internal", f"Failed to read IPC stream: {exc}") from None
        try:
            ds = write_dataset(table, os.path.join(self.owner._base, dir_name),
                               mode="overwrite" if mode == "overwrite" else "create")
        except Exception as exc:  # noqa: BLE001
            raise ns_error("Internal", f"Failed to write dataset: {exc}") from None
        response = {"version": ds.version, "location": table_uri, "storage_options": self.owner._storage_options,
                    "properties": properties}
        if overwriting:
            self.insert([_Row(object_id, "table", dir_name, _metadata_json(properties))], upsert=True)
        elif existing is not None:
            response["properties"] = _parse_metadata(existing.metadata, "table", object_id)
        else:
            self.insert([_Row(object_id, "table", dir_name, _metadata_json(properties))])
        return response

    def drop_table(self, req: Dict[str, Any]) -> Dict[str, Any]:
        table_id = req.get("id")
        if table_id is None:
            raise ns_error("InvalidInput", "Table ID is required")
        if not table_id:
            raise ns_error("InvalidInput", "Table ID cannot be empty")
        namespace, name = _split_id(table_id)
        object_id = _object_id(table_id)
        info = self.table_info(object_id)
        if info is None:
            raise ns_error("TableNotFound", name)
        self.delete(object_id)
        shutil.rmtree(os.path.join(self.owner._base, info.location), ignore_errors=True)
        return {"id": table_id, "location": construct_full_uri(self.owner._root, info.location)}

    def list_namespaces(self, req: Dict[str, Any]) -> Dict[str, Any]:
        parent = req.get("id")
        if parent is None:
            raise ns_error("InvalidInput", "Namespace ID is required")
        names = [r.object_id.split(_DELIMITER)[-1] for r in self._children(self.rows(), parent, "namespace")]
        token = _paginate(names, req.get("page_token"), req.get("limit"))
        return {"namespaces": names, "page_token": token}

    def describe_namespace(self, req: Dict[str, Any]) -> Dict[str, Any]:
        namespace = req.get("id")
        if namespace is None:
            raise ns_error("InvalidInput", "Namespace ID is required")
        if not namespace:
            return {"properties": {}}
        object_id = _object_id(namespace)
        info = self.namespace_info(object_id)
        if info is None:
            raise ns_error("NamespaceNotFound", object_id)
        return {"properties": _parse_metadata(info.metadata, "namespace", object_id)}

    def create_namespace(self, req: Dict[str, Any]) -> Dict[str, Any]:
        namespace = req.get("id")
        if namespace is None:
            raise ns_error("InvalidInput", "Namespace ID is required")
        if not namespace:
            raise ns_error("NamespaceAlreadyExists", "root namespace")
        if len(namespace) > 1:
            self.validate_namespace_levels(namespace[:-1])
        object_id = _object_id(namespace)
        if self.contains(object_id):
            raise ns_error("NamespaceAlreadyExists", object_id)
        properties = req.get("properties")
        self.insert([_Row(object_id, "namespace", None, _metadata_json(properties))])
        return {"properties": properties}

    def drop_namespace(self, req: Dict[str, Any]) -> Dict[str, Any]:
        namespace = req.get("id")
        if namespace is None:
            raise ns_error("InvalidInput", "Namespace ID is required")
        if not namespace:
            raise ns_error("InvalidInput", "Root namespace cannot be dropped")
        object_id = _object_id(namespace)
        rows = self.rows()
        if not any(r.object_id == object_id for r in rows):
            raise ns_error("NamespaceNotFound", object_id)
        prefix = object_id + _DELIMITER
        count = sum(1 for r in rows if r.object_id.startswith(prefix))
        if count > 0:
            raise ns_error("NamespaceNotEmpty", f"'{object_id}' (contains {count} child objects)")
        self.delete(object_id)
        return {}

    def namespace_exists(self, req: Dict[str, Any]) -> None:
        namespace = req.get("id")
        if namespace is None:
            raise ns_error("InvalidInput", "Namespace ID is required")
        if namespace and not self.contains(_object_id(namespace)):
            raise ns_error("NamespaceNotFound", _object_id(namespace))

    def declare_table(self, req: Dict[str, Any]) -> Dict[str, Any]:
        table_id = req.get("id")
        if table_id is None:
            raise ns_error("InvalidInput", "Table ID is required")
        if not table_id:
            raise ns_error("InvalidInput", "Table ID cannot be empty")
        namespace, name = _split_id(table_id)
        object_id = _object_id(table_id)
        if self.table_info(object_id) is not None:
            raise ns_error("TableAlreadyExists", name)
        if not namespace and self.owner._dir_listing_enabled:
            dir_name = f"{name}.lance"
        else:
            dir_name = _generate_dir_name(object_id)
        table_uri = construct_full_uri(self.owner._root, dir_name)
        location = req.get("location")
        if location is not None and location.rstrip("/") != table_uri:
            raise ns_error("InvalidInput", f"Cannot declare table {name} at location {location.rstrip('/')}, "
                                           f"must be at location {table_uri}")
        self.ensure_writable()
        if not _put_marker_atomic(os.path.join(self.owner._base, dir_name, ".lance-reserved")):
            raise ns_error("TableAlreadyExists", name)
        properties = req.get("properties")
        self.insert([_Row(object_id, "table", dir_name, _metadata_json(properties))])
        vend = req.get("vend_credentials")
        return {"location": table_uri, "storage_options": self.owner._storage_options if vend is None or vend else None,
                "properties": properties}

    def register_table(self, req: Dict[str, Any]) -> Dict[str, Any]:
        table_id = req.get("id")
        if table_id is None:
            raise ns_error("InvalidInput", "Table ID is required")
        if not table_id:
            raise ns_error("InvalidInput", "Table ID cannot be empty")
        location = req.get("location") or ""
        rule = "Location must be a relative path within the root directory"
        if "://" in location:
            raise ns_error("InvalidInput", f"Absolute URIs are not allowed for register_table. {rule}: {location}")
        if location.startswith("/"):
            raise ns_error("InvalidInput", f"Absolute paths are not allowed for register_table. {rule}: {location}")
        if ".." in location:
            raise ns_error("InvalidInput", f"Path traversal is not allowed. {rule}: {location}")
        namespace, _ = _split_id(table_id)
        object_id = _object_id(table_id)
        if namespace:
            self.validate_namespace_levels(namespace)
        if self.contains(object_id):
            raise ns_error("TableAlreadyExists", object_id)
        self.insert([_Row(object_id, "table", location)])
        return {"location": location}

    def deregister_table(self, req: Dict[str, Any]) -> Dict[str, Any]:
        table_id = req.get("id")
        if table_id is None:
            raise ns_error("InvalidInput", "Table ID is required")
        if not table_id:
            raise ns_error("InvalidInput", "Table ID cannot be empty")
        object_id = _object_id(table_id)
        info = self.table_info(object_id)
        if info is None:
            raise ns_error("TableNotFound", object_id)
        self.delete(object_id)
        return {"id": table_id, "location": construct_full_uri(self.owner._root, info.location)}

    def table_path(self, table_id: List[str]) -> Tuple[str, str]:
        """(local path, URI) of a table the manifest records."""
        object_id = _object_id(table_id)
        info = self.table_info(object_id)
        if info is None:
            raise ns_error("TableNotFound", object_id)
        return os.path.join(self.owner._base, info.location), construct_full_uri(self.owner._root, info.location)


def _create_mode(mode: Optional[str]) -> str:
    if mode is None or mode.lower() == "create":
        return "create"
    if mode.lower() in ("existok", "exist_ok"):
        return "exist_ok"
    if mode.lower() == "overwrite":
        return "overwrite"
    raise ns_error("InvalidInput",
                   f"Unsupported create_table mode '{mode}'. Supported modes are: 'Create', 'ExistOk', 'Overwrite'")


def _json_schema(schema: pa.Schema) -> Dict[str, Any]:
    """An Arrow schema as the namespace spec's JsonArrowSchema (lance_namespace::schema)."""
    return {"fields": [_json_field(f) for f in schema],
            "metadata": {k.decode(): v.decode() for k, v in (schema.metadata or {}).items()} or None}


def _json_field(field: pa.Field) -> Dict[str, Any]:
    out = {"name": field.name, "nullable": field.nullable, "type": _json_type(field.type)}
    if field.metadata:
        out["metadata"] = {k.decode(): v.decode() for k, v in field.metadata.items()}
    return out


def _json_type(t: pa.DataType) -> Dict[str, Any]:
    simple = {
        pa.null(): "null", pa.bool_(): "bool", pa.int8(): "int8", pa.uint8(): "uint8", pa.int16(): "int16",
        pa.uint16(): "uint16", pa.int32(): "int32", pa.uint32(): "uint32", pa.int64(): "int64",
        pa.uint64(): "uint64", pa.float16(): "float16", pa.float32(): "float32", pa.float64(): "float64",
        pa.utf8(): "utf8", pa.large_utf8(): "large_utf8", pa.binary(): "binary",
        pa.large_binary(): "large_binary", pa.date32(): "date32", pa.date64(): "date64",
    }
    if t in simple:
        return {"type": simple[t]}
    if pa.types.is_fixed_size_list(t):
        return {"type": "fixed_size_list", "fields": [_json_field(t.value_field)], "length": t.list_size}
    if pa.types.is_large_list(t):
        return {"type": "large_list", "fields": [_json_field(t.value_field)]}
    if pa.types.is_list(t):
        return {"type": "list", "fields": [_json_field(t.value_field)]}
    if pa.types.is_struct(t):
        return {"type": "struct", "fields": [_json_field(t.field(i)) for i in range(t.num_fields)]}
    if pa.types.is_fixed_size_binary(t):
        return {"type": "fixed_size_binary", "length": t.byte_width}
    if pa.types.is_timestamp(t):
        unit = {"s": "second", "ms": "millisecond", "us": "microsecond", "ns": "nanosecond"}[t.unit]
        return {"type": f"timestamp", "unit": unit, "timezone": t.tz} if t.tz else {"type": "timestamp", "unit": unit}
    if pa.types.is_decimal(t):
        return {"type": f"decimal{t.bit_width}", "precision": t.precision, "scale": t.scale}
    return {"type": str(t)}


class PyDirectoryNamespace:
    """Lance's DirectoryNamespace: tables as Lance datasets under a root directory."""

    def __init__(self, session=None, context_provider=None, **properties):
        from nanolance.lance.dataset import _path_of

        props = {str(k): str(v) for k, v in properties.items()}
        root = props.get("root")
        if root is None:
            raise ns_error("InvalidInput", "Missing required property 'root' for directory namespace")
        self._root = root.rstrip("/")
        storage = {k[len("storage."):]: v for k, v in props.items() if k.startswith("storage.")}
        self._storage_options = storage or None

        def flag(name: str, default: bool) -> bool:
            value = _str_to_bool(props.get(name))
            return default if value is None else value

        self._manifest_enabled = flag("manifest_enabled", True)
        self._dir_listing_enabled = flag("dir_listing_enabled", True)
        self._inline_optimization_enabled = flag("inline_optimization_enabled", False)
        self._table_version_tracking_enabled = flag("table_version_tracking_enabled", False)
        self._migration_enabled = flag("dir_listing_to_manifest_migration_enabled", False)
        self._vend_input_storage_options = flag("vend_input_storage_options", False)
        refresh = props.get("vend_input_storage_options_refresh_interval_millis")
        self._vend_refresh_millis = int(refresh) if refresh is not None and refresh.isdigit() else None
        retries = props.get("commit_retries")
        self._commit_retries = int(retries) if retries is not None and retries.isdigit() else None
        self._metrics: Optional[Dict[str, int]] = {} if flag("ops_metrics_enabled", False) else None
        self._metrics_lock = threading.Lock()
        self._context_provider = context_provider
        self._base = _path_of(self._root)
        self._manifest_ns = _ManifestNamespace(self)
        if self._manifest_enabled and self._manifest_exists():
            self._manifest_ns._ensure_readable(self._manifest_ns._dataset())

    # ── plumbing ──────────────────────────────────────────────────────────────────────────────────

    def namespace_id(self) -> str:
        return "DirectoryNamespace { root: " + json.dumps(self._root) + " }"

    def _record(self, operation: str) -> None:
        if self._metrics is not None:
            with self._metrics_lock:
                self._metrics[operation] = self._metrics.get(operation, 0) + 1

    def retrieve_ops_metrics(self) -> Dict[str, int]:
        if self._metrics is None:
            return {}
        with self._metrics_lock:
            return dict(self._metrics)

    def reset_ops_metrics(self) -> None:
        if self._metrics is not None:
            with self._metrics_lock:
                self._metrics.clear()

    def _manifest_exists(self) -> bool:
        return _has_manifests(os.path.join(self._base, _MANIFEST))

    def _manifest_for_read(self) -> Optional[_ManifestNamespace]:
        if self._manifest_enabled and self._manifest_exists():
            return self._manifest_ns
        return None

    def _manifest_for_write(self) -> Optional[_ManifestNamespace]:
        if not self._manifest_enabled:
            return None
        self._manifest_ns.ensure_up_to_date()
        return self._manifest_ns

    def _child_requires_manifest(self) -> Exception:
        if self._manifest_enabled:
            return ns_error("NamespaceNotFound", "Child namespace reads require an existing __manifest dataset")
        return ns_error("Unsupported", "Child namespaces are only supported when manifest mode is enabled")

    @staticmethod
    def _table_name(table_id: Optional[List[str]]) -> str:
        if table_id is None:
            raise ns_error("InvalidInput", "Directory namespace table ID cannot be empty")
        if len(table_id) != 1:
            raise ns_error("Unsupported", "Multi-level table IDs are only supported when manifest mode is enabled, "
                                          f"but got: {_debug_list(table_id)}")
        return table_id[0]

    def _table_dir(self, name: str) -> str:
        return os.path.join(self._base, f"{name}.lance")

    def _table_full_uri(self, name: str) -> str:
        return f"{self._root}/{name}.lance"

    def _table_status(self, name: str) -> Tuple[bool, bool, bool]:
        """(exists, deregistered, reserved) of a root table directory."""
        try:
            entries = os.listdir(self._table_dir(name))
        except (FileNotFoundError, NotADirectoryError):
            return False, False, False
        return (bool(entries), any(e.endswith(".lance-deregistered") for e in entries),
                any(e.endswith(".lance-reserved") for e in entries))

    def _list_directory_tables(self) -> List[str]:
        try:
            names = sorted(os.listdir(self._base))
        except FileNotFoundError:
            return []
        out = []
        for entry in names:
            if entry.endswith(".lance"):
                name = entry[: -len(".lance")]
                if not self._table_status(name)[1]:
                    out.append(name)
        return out

    def _filter_declared(self, tables: List[str], include_declared: bool) -> List[str]:
        if include_declared:
            return tables
        return [t for t in tables if _has_manifests(self._table_dir(t))]

    def _storage_options_for(self, vend: bool) -> Optional[Dict[str, str]]:
        if self._vend_input_storage_options:
            options = dict(self._storage_options or {})
            if self._vend_refresh_millis is not None:
                options["expires_at_millis"] = str(int(time.time() * 1000) + self._vend_refresh_millis)
            return options
        return None

    def _open(self, path: str, version=None, operation: Optional[str] = None, uri: Optional[str] = None):
        from nanolance.lance.dataset import LanceDataset

        if version is not None and int(version) < 0:
            raise ns_error("InvalidInput", f"Table version for {operation} must be non-negative, got {version}")
        if operation is None:
            return LanceDataset(path, version=version)
        if not _has_manifests(path):
            raise ns_error("TableNotFound", f"Failed to open table at '{uri}' for {operation}: Dataset at path "
                                            f"{_store_path(path)} was not found")
        try:
            return LanceDataset(path, version=version)
        except Exception as exc:  # noqa: BLE001
            if version is not None:
                raise ns_error("TableVersionNotFound", f"Failed to checkout version {version} for table at '{uri}' "
                                                       f"during {operation}: {exc}") from None
            raise ns_error("TableNotFound", f"Failed to open table at '{uri}' for {operation}: {exc}") from None

    def _resolve(self, table_id) -> Tuple[str, str]:
        """(local path, URI) of a table: its describe_table location."""
        response = self._describe_table({"id": table_id, "load_detailed_metadata": False})
        uri = response.get("location")
        if uri is None:
            raise ns_error("TableNotFound", f"Table location not found for: {_debug_opt_list(table_id)}")
        return self._local_path_of_uri(uri), uri

    def _local_path_of_uri(self, uri: str) -> str:
        from urllib.parse import unquote

        from nanolance.lance.dataset import _path_of

        if uri.startswith("file://"):
            return unquote(uri[len("file://"):])
        return _path_of(uri)

    # ── namespaces ────────────────────────────────────────────────────────────────────────────────

    def list_namespaces(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("list_namespaces")
        manifest = self._manifest_for_read()
        if manifest is not None:
            return manifest.list_namespaces(req)
        if req.get("id"):
            raise self._child_requires_manifest()
        return {"namespaces": []}

    def describe_namespace(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("describe_namespace")
        manifest = self._manifest_for_read()
        if manifest is not None:
            return manifest.describe_namespace(req)
        if req.get("id"):
            raise self._child_requires_manifest()
        return {"properties": {}}

    def create_namespace(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("create_namespace")
        manifest = self._manifest_for_write()
        if manifest is not None:
            return manifest.create_namespace(req)
        if not req.get("id"):
            raise ns_error("NamespaceAlreadyExists", "root namespace")
        raise ns_error("Unsupported", "Child namespaces are only supported when manifest mode is enabled")

    def drop_namespace(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("drop_namespace")
        manifest = self._manifest_for_write()
        if manifest is not None:
            return manifest.drop_namespace(req)
        if not req.get("id"):
            raise ns_error("InvalidInput", "Root namespace cannot be dropped")
        raise ns_error("Unsupported", "Child namespaces are only supported when manifest mode is enabled")

    def namespace_exists(self, request) -> None:
        req = _as_dict(request)
        self._record("namespace_exists")
        manifest = self._manifest_for_read()
        if manifest is not None:
            return manifest.namespace_exists(req)
        if req.get("id"):
            raise self._child_requires_manifest()
        return None

    # ── tables ────────────────────────────────────────────────────────────────────────────────────

    def list_tables(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("list_tables")
        namespace = req.get("id")
        if namespace is None:
            raise ns_error("InvalidInput", "Namespace ID is required")
        manifest = self._manifest_for_read()
        if namespace:
            if manifest is not None:
                return manifest.list_tables(req)
            raise self._child_requires_manifest()
        if manifest is not None and not self._dir_listing_enabled:
            return manifest.list_tables(req)
        if not self._dir_listing_enabled:
            return {"tables": []}
        if manifest is not None and self._migration_enabled:
            locations = manifest.table_locations()
            tables = manifest.list_tables(dict(req, limit=None, page_token=None))["tables"]
            for name in self._list_directory_tables():
                if f"{self._root}/{name}.lance" not in locations and f"{name}.lance" not in locations:
                    tables.append(name)
        else:
            tables = self._list_directory_tables()
        include = req.get("include_declared")
        tables = self._filter_declared(tables, True if include is None else bool(include))
        token = _paginate(tables, req.get("page_token"), req.get("limit"))
        return {"tables": tables, "page_token": token}

    def list_all_tables(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        include = req.get("include_declared")
        tables = self._filter_declared(self._list_directory_tables(), True if include is None else bool(include))
        _paginate(tables, req.get("page_token"), req.get("limit"))
        return {"tables": tables}

    def describe_table(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("describe_table")
        return self._describe_table(req)

    def _describe_table(self, req: Dict[str, Any]) -> Dict[str, Any]:
        table_id = req.get("id")
        root_level = table_id is not None and len(table_id) == 1
        child = table_id is not None and len(table_id) > 1
        skip_manifest = self._dir_listing_enabled and root_level and not self._migration_enabled
        manifest = self._manifest_for_read()
        managed = True if self._table_version_tracking_enabled else None
        if manifest is not None and not skip_manifest:
            try:
                response = manifest.describe_table(req)
            except Exception as exc:  # noqa: BLE001
                if not (self._dir_listing_enabled and root_level and _is_ns_error(exc)
                        and type(exc).__name__ == "TableNotFoundError"):
                    raise
            else:
                vend = req.get("vend_credentials")
                response["storage_options"] = self._storage_options_for(vend is None or bool(vend))
                if managed:
                    response["managed_versioning"] = True
                return response
        if child:
            raise self._child_requires_manifest()
        name = self._table_name(table_id)
        formatted = _format_table_id(table_id)
        if not self._dir_listing_enabled:
            raise ns_error("TableNotFound", formatted)
        table_uri = self._table_full_uri(name)
        exists, deregistered, reserved = self._table_status(name)
        if not exists:
            raise ns_error("TableNotFound", formatted)
        if deregistered:
            raise ns_error("TableNotFound", f"Table is deregistered: {formatted}")
        detailed = bool(req.get("load_detailed_metadata"))
        check_declared = detailed or bool(req.get("check_declared"))
        if check_declared:
            is_only_declared = (not _has_manifests(self._table_dir(name))) if reserved else False
        else:
            is_only_declared = None
        vend = req.get("vend_credentials")
        out = {"table": name, "namespace": list(table_id[:-1]), "location": table_uri, "table_uri": table_uri,
               "storage_options": self._storage_options_for(vend is None or bool(vend)),
               "is_only_declared": is_only_declared, "managed_versioning": managed}
        if not detailed or is_only_declared:
            return out
        try:
            ds = self._open(self._table_dir(name))
        except Exception as exc:  # noqa: BLE001
            raise ns_error("Internal", f"Table directory exists but cannot load dataset {name}: {exc}") from None
        if req.get("version") is not None:
            try:
                ds = ds.checkout_version(int(req["version"]))
            except Exception as exc:  # noqa: BLE001
                raise ns_error("TableVersionNotFound",
                               f"Version {req['version']} not found for table '{name}': {exc}") from None
        out.update(version=ds.version, schema=_json_schema(ds.schema),
                   metadata={k: v for k, v in (ds.metadata or {}).items()} if hasattr(ds, "metadata") else {})
        return out

    def table_exists(self, request) -> None:
        req = _as_dict(request)
        self._record("table_exists")
        table_id = req.get("id")
        root_level = table_id is not None and len(table_id) == 1
        child = table_id is not None and len(table_id) > 1
        skip_manifest = self._dir_listing_enabled and root_level and not self._migration_enabled
        manifest = self._manifest_for_read()
        if manifest is not None and not skip_manifest:
            try:
                manifest.table_exists(req)
                return None
            except Exception as exc:  # noqa: BLE001
                if not (self._dir_listing_enabled and root_level and type(exc).__name__ == "TableNotFoundError"):
                    raise
        if child:
            raise self._child_requires_manifest()
        name = self._table_name(table_id)
        formatted = _format_table_id(table_id)
        if not self._dir_listing_enabled:
            raise ns_error("TableNotFound", formatted)
        exists, deregistered, _ = self._table_status(name)
        if not exists:
            raise ns_error("TableNotFound", formatted)
        if deregistered:
            raise ns_error("TableNotFound", f"Table is deregistered: {formatted}")
        return None

    def drop_table(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("drop_table")
        manifest = self._manifest_for_write()
        if manifest is not None:
            return manifest.drop_table(req)
        name = self._table_name(req.get("id"))
        shutil.rmtree(self._table_dir(name), ignore_errors=True)
        return {"id": req.get("id"), "location": self._table_full_uri(name)}

    def create_table(self, request, request_data: bytes) -> Dict[str, Any]:
        from nanolance.lance.dataset import write_dataset

        req = _as_dict(request)
        self._record("create_table")
        manifest = self._manifest_for_write()
        if manifest is not None:
            return manifest.create_table(req, request_data)
        if req.get("properties"):
            raise ns_error("Unsupported", "create_table with non-empty table properties requires manifest_enabled=true")
        name = self._table_name(req.get("id"))
        table = _ipc_table(request_data, "create_table")
        if self._table_status(name)[0] and _has_manifests(self._table_dir(name)):
            raise ns_error("TableAlreadyExists", name)
        try:
            write_dataset(table, self._table_dir(name), mode="create")
        except Exception as exc:  # noqa: BLE001
            if _has_manifests(self._table_dir(name)):
                raise ns_error("TableAlreadyExists", name) from None
            raise ns_error("Internal", f"Failed to write table at '{self._table_full_uri(name)}': {exc}") from None
        return {"version": 1, "location": self._table_full_uri(name), "storage_options": self._storage_options,
                "properties": req.get("properties")}

    def declare_table(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("declare_table")
        vend = req.get("vend_credentials")
        manifest = self._manifest_for_write()
        if manifest is not None:
            response = manifest.declare_table(req)
            response["storage_options"] = self._storage_options_for(vend is None or bool(vend))
            if self._table_version_tracking_enabled:
                response["managed_versioning"] = True
            return response
        if req.get("properties"):
            raise ns_error("Unsupported", "declare_table with non-empty table properties requires manifest_enabled=true")
        name = self._table_name(req.get("id"))
        table_uri = self._table_full_uri(name)
        location = req.get("location")
        if location is not None and location.rstrip("/") != table_uri:
            raise ns_error("InvalidInput", f"Cannot declare table {name} at location {location.rstrip('/')}, "
                                           f"must be at location {table_uri}")
        exists, _, reserved = self._table_status(name)
        if exists and not reserved:
            raise ns_error("TableAlreadyExists", name)
        if not _put_marker_atomic(os.path.join(self._table_dir(name), ".lance-reserved")):
            raise ns_error("TableAlreadyExists", name)
        return {"location": table_uri, "storage_options": self._storage_options_for(vend is None or bool(vend)),
                "properties": req.get("properties"),
                "managed_versioning": True if self._table_version_tracking_enabled else None}

    def register_table(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("register_table")
        manifest = self._manifest_for_write()
        if manifest is not None:
            return manifest.register_table(req)
        raise ns_error("Unsupported", "register_table is only supported when manifest mode is enabled")

    def deregister_table(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("deregister_table")
        manifest = self._manifest_for_write()
        if manifest is not None:
            return manifest.deregister_table(req)
        name = self._table_name(req.get("id"))
        exists, deregistered, _ = self._table_status(name)
        if not exists:
            raise ns_error("TableNotFound", name)
        if deregistered:
            raise ns_error("TableNotFound", f"Table is already deregistered: {name}")
        if not _put_marker_atomic(os.path.join(self._table_dir(name), ".lance-deregistered")):
            raise ns_error("InvalidTableState", f"Table is already deregistered: {name}")
        return {"id": req.get("id"), "location": self._table_full_uri(name)}

    def rename_table(self, request) -> Dict[str, Any]:
        raise ns_error("Unsupported", "rename_table is not supported by DirectoryNamespace")

    # ── versions ──────────────────────────────────────────────────────────────────────────────────

    @staticmethod
    def _check_branch(req: Dict[str, Any]) -> None:
        branch = req.get("branch")
        if branch is not None and branch not in ("", "main"):
            raise ns_error("Unsupported", "table branches (nanolance does not implement them)")

    @staticmethod
    def _list_versions(path: str, descending: bool, limit: Optional[int]) -> List[Dict[str, Any]]:
        versions_dir = os.path.join(path, "_versions")
        out = []
        try:
            entries = list(os.scandir(versions_dir))
        except FileNotFoundError:
            entries = []
        for entry in entries:
            version = _manifest_version_from_filename(entry.name)
            if version is None:
                continue
            stat = entry.stat()
            out.append({"version": version, "manifest_path": _store_path(entry.path),
                        "manifest_size": stat.st_size, "e_tag": _e_tag(stat),
                        "timestamp_millis": int(stat.st_mtime * 1000), "metadata": None})
        out.sort(key=lambda v: v["version"], reverse=descending)
        if limit is not None and limit >= 0:
            out = out[:limit]
        return out

    def list_table_versions(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("list_table_versions")
        self._check_branch(req)
        path, _ = self._resolve(req.get("id"))
        return {"versions": self._list_versions(path, req.get("descending") is True, req.get("limit")),
                "page_token": None}

    def describe_table_version(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("describe_table_version")
        self._check_branch(req)
        path, _ = self._resolve(req.get("id"))
        versions = self._list_versions(path, True, None)
        wanted = req.get("version")
        formatted = _format_table_id(req["id"]) if req.get("id") is not None else "table id '<unknown>'"
        if wanted is not None:
            match = [v for v in versions if v["version"] == wanted]
            if not match:
                raise ns_error("TableVersionNotFound", f"version {wanted} for table {formatted}")
            return {"version": match[0]}
        if not versions:
            raise ns_error("TableVersionNotFound", f"latest version for table {formatted}")
        return {"version": versions[0]}

    def create_table_version(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("create_table_version")
        self._check_branch(req)
        path, table_uri = self._resolve(req.get("id"))
        version = int(req["version"])
        staging = "/" + str(req["manifest_path"]).lstrip("/")
        name = (str(version) if req.get("naming_scheme") == "V1" else f"{_U64_MAX - version:020d}") + ".manifest"
        final = os.path.join(path, "_versions", name)
        if os.path.exists(final):
            return self._existing_version(staging, final, version, table_uri, req.get("manifest_size"))
        latest = self._list_versions(path, True, 1)
        expected = latest[0]["version"] + 1 if latest else 1
        if version != expected:
            shown = str(latest[0]["version"]) if latest else "none"
            raise ns_error("ConcurrentModification", f"Version CAS failed for table at '{table_uri}': requested "
                                                     f"{version}, expected {expected} (latest {shown})")
        try:
            with open(staging, "rb") as f:
                data = f.read()
        except FileNotFoundError:
            raise ns_error("InvalidInput", f"Staging manifest not found at '{req['manifest_path']}' for version "
                                           f"{version} of table at '{table_uri}'") from None
        try:
            fd = os.open(final, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        except FileExistsError:
            return self._existing_version(staging, final, version, table_uri, req.get("manifest_size"))
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        try:
            os.remove(staging)
        except OSError:
            pass
        return _version_response(version, final)

    @staticmethod
    def _existing_version(staging: str, final: str, version: int, table_uri: str, size) -> Dict[str, Any]:
        final_size = os.path.getsize(final)
        same = False
        if size is None or int(size) == final_size:
            try:
                with open(staging, "rb") as a, open(final, "rb") as b:
                    same = a.read() == b.read()
            except FileNotFoundError:
                same = False
        if not same:
            raise ns_error("ConcurrentModification",
                           f"Version {version} already exists for table at '{table_uri}' with different content")
        try:
            os.remove(staging)
        except OSError:
            pass
        return _version_response(version, final)

    def batch_delete_table_versions(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("batch_delete_table_versions")
        self._check_branch(req)
        ranges = [(int(r["start_version"]), int(r["end_version"])) for r in (req.get("ranges") or [])]
        requested = sum(0 if e < 0 else max(e - s, 0) for s, e in ranges)
        if requested > 1_000_000:
            raise ns_error("InvalidInput", f"batch_delete requested {requested} versions; limit is 1000000")
        path, _ = self._resolve(req.get("id"))
        deleted = 0
        for version in self._list_versions(path, False, None):
            v = version["version"]
            if any(s <= v and (e < 0 or v <= e) for s, e in ranges):
                try:
                    os.remove("/" + version["manifest_path"])
                    deleted += 1
                except OSError:
                    pass
        return {"deleted_count": deleted, "transaction_id": None}

    # ── data ──────────────────────────────────────────────────────────────────────────────────────

    def count_table_rows(self, request) -> int:
        req = _as_dict(request)
        self._record("count_table_rows")
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, req.get("version"), "count_table_rows", uri)
        try:
            return int(ds.count_rows(req.get("predicate")))
        except Exception as exc:  # noqa: BLE001
            raise ns_error("Internal", f"Failed to count rows for table at '{uri}': {exc}") from None

    def _write(self, path: str, uri: str, table: pa.Table, mode: str):
        from nanolance.lance.dataset import write_dataset

        try:
            return write_dataset(table, path, mode=mode)
        except Exception as exc:  # noqa: BLE001
            raise ns_error("Internal", f"Failed to write table at '{uri}': {exc}") from None

    def insert_into_table(self, request, request_data: bytes) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("insert_into_table")
        path, uri = self._resolve(req.get("id"))
        table = _ipc_table(request_data, "insert_into_table")
        mode = req.get("mode")
        if mode is None or mode.lower() == "append":
            mode = "append"
        elif mode.lower() == "overwrite":
            mode = "overwrite"
        else:
            raise ns_error("InvalidInput", f"Unsupported write mode '{mode}'. Supported modes are: 'append', "
                                           "'overwrite'")
        self._write(path, uri, table, mode if _has_manifests(path) else "create")
        return {"transaction_id": None}

    def merge_insert_into_table(self, request, request_data: bytes) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("merge_insert_into_table")
        path, uri = self._resolve(req.get("id"))
        on = req.get("on")
        if not on:
            raise ns_error("InvalidInput", "merge_insert_into_table requires 'on' column(s)")
        keys = [c.strip() for c in on.split(",") if c.strip()] if isinstance(on, str) else list(on)
        table = _ipc_table(request_data, "merge_insert_into_table")
        if not _has_manifests(path):
            ds = self._write(path, uri, table, "create")
            return {"transaction_id": None, "num_updated_rows": 0, "num_inserted_rows": table.num_rows,
                    "num_deleted_rows": 0, "version": ds.version}
        ds = self._open(path, None, "merge_insert_into_table", uri)
        builder = ds.merge_insert(keys)
        if req.get("when_matched_update_all_filt"):
            builder = builder.when_matched_update_all(req["when_matched_update_all_filt"])
        elif req.get("when_matched_update_all"):
            builder = builder.when_matched_update_all()
        if req.get("when_not_matched_insert_all") is not False:
            builder = builder.when_not_matched_insert_all()
        if req.get("when_not_matched_by_source_delete_filt"):
            builder = builder.when_not_matched_by_source_delete(req["when_not_matched_by_source_delete_filt"])
        elif req.get("when_not_matched_by_source_delete"):
            builder = builder.when_not_matched_by_source_delete()
        if req.get("use_index") is not None:
            builder = builder.use_index(bool(req["use_index"]))
        stats = self._mutate(lambda: builder.execute(table), "merge_insert_into_table", uri)
        ds.checkout_latest()
        return {"transaction_id": None, "num_updated_rows": int(stats.get("num_updated_rows", 0)),
                "num_inserted_rows": int(stats.get("num_inserted_rows", 0)),
                "num_deleted_rows": int(stats.get("num_deleted_rows", 0)), "version": ds.version}

    @staticmethod
    def _mutate(fn, operation: str, uri: str):
        try:
            return fn()
        except Exception as exc:  # noqa: BLE001
            if _is_ns_error(exc):
                raise
            text = str(exc)
            if "conflict" in text.lower():
                raise ns_error("ConcurrentModification",
                               f"Failed to run {operation} on table at '{uri}': {text}") from None
            if isinstance(exc, (ValueError, KeyError)):
                raise ns_error("InvalidInput", f"Failed to run {operation} on table at '{uri}': {text}") from None
            raise ns_error("Internal", f"Failed to run {operation} on table at '{uri}': {text}") from None

    def update_table(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("update_table")
        updates = req.get("updates") or []
        if not updates:
            raise ns_error("InvalidInput", "update_table requires at least one [column, expression] pair")
        seen = set()
        for i, pair in enumerate(updates):
            if len(pair) != 2:
                raise ns_error("InvalidInput", f"update_table updates[{i}] must be a [column, expression] pair, "
                                               f"got {len(pair)} elements")
            if not pair[0].strip():
                raise ns_error("InvalidInput", f"update_table updates[{i}] has an empty column name")
            if pair[0] in seen:
                raise ns_error("InvalidInput", f"update_table cannot update column '{pair[0]}' more than once")
            seen.add(pair[0])
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, None, "update_table", uri)
        predicate = req.get("predicate")
        where = predicate if predicate and predicate.strip() else None
        result = self._mutate(lambda: ds.update({c: e for c, e in updates}, where=where), "update_table", uri)
        return {"transaction_id": None, "updated_rows": int(result.get("num_rows_updated", 0)),
                "version": ds.version, "properties": None}

    def delete_from_table(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("delete_from_table")
        predicate = req.get("predicate") or ""
        if not predicate.strip():
            raise ns_error("InvalidInput", "delete_from_table requires a non-empty predicate")
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, None, "delete_from_table", uri)
        self._mutate(lambda: ds.delete(predicate), "delete_from_table", uri)
        return {"transaction_id": None, "version": ds.version}

    def query_table(self, request) -> bytes:
        req = _as_dict(request)
        self._record("query_table")
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, req.get("version"), "query_table", uri)
        try:
            table = _run_query(ds, req, apply_offset_without_vector=True)
        except Exception as exc:  # noqa: BLE001
            if _is_ns_error(exc):
                raise
            raise ns_error("Internal", f"Failed to execute query: {exc}") from None
        sink = io.BytesIO()
        with pa.ipc.new_file(sink, table.schema) as writer:
            writer.write_table(table)
        return sink.getvalue()

    def explain_table_query_plan(self, request) -> str:
        req = _as_dict(request)
        query = req.get("query") or {}
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, query.get("version"), "explain_table_query_plan", uri)
        scanner = ds.scanner(**_scanner_args(query))
        return scanner.explain_plan(bool(req.get("verbose")))

    def analyze_table_query_plan(self, request) -> str:
        req = _as_dict(request)
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, req.get("version"), "analyze_table_query_plan", uri)
        return ds.scanner(**_scanner_args(req)).analyze_plan()

    # ── indexes ───────────────────────────────────────────────────────────────────────────────────

    def create_table_index(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("create_table_index")
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, None, "create_table_index", uri)
        index_type = _parse_index_type(req.get("index_type") or "")
        name = req.get("name")
        column = req.get("column")
        try:
            if index_type in _SCALAR_INDEX_TYPES:
                kwargs = {}
                if index_type == "INVERTED":
                    for key in ("with_position", "base_tokenizer", "language", "max_token_length", "lower_case",
                                "stem", "remove_stop_words", "ascii_folding"):
                        if req.get(key) is not None:
                            kwargs[key] = req[key]
                ds.create_scalar_index(column, index_type, name=name, **kwargs)
            else:
                metric = _parse_metric(req.get("distance_type"))
                ds.create_index(column, index_type, name=name, metric=metric)
        except Exception as exc:  # noqa: BLE001
            text = str(exc)
            shown = name or "<auto-generated>"
            if "already exists" in text:
                raise ns_error("TableIndexAlreadyExists",
                               f"Index '{shown}' already exists on table '{uri}': {text}") from None
            if "not found" in text or "does not exist" in text:
                raise ns_error("TableColumnNotFound", f"Column '{column}' not found for table '{uri}': {text}") \
                    from None
            raise ns_error("Internal", f"Failed to create {req.get('index_type')} index '{shown}' on column "
                                       f"'{column}' for table '{uri}': {text}") from None
        return {"transaction_id": _transaction_uuid(ds)}

    def create_table_scalar_index(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("create_table_scalar_index")
        if _parse_index_type(req.get("index_type") or "") not in _SCALAR_INDEX_TYPES:
            raise ns_error("InvalidInput", "create_table_scalar_index only supports scalar index types, got "
                                           f"{req.get('index_type')}")
        return {"transaction_id": self.create_table_index(req).get("transaction_id")}

    def list_table_indices(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("list_table_indices")
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, req.get("version"), "list_table_indices", uri)
        total = ds.count_rows()
        out = []
        for d in ds.describe_indices():
            indexed = int(d.num_rows_indexed) if getattr(d, "num_rows_indexed", None) is not None else 0
            segments = list(getattr(d, "segments", []) or [])
            out.append({
                "index_name": d.name, "index_uuid": str(segments[0].uuid) if segments else "",
                "columns": list(d.field_names) if hasattr(d, "field_names") else [],
                "status": "SUCCEEDED", "index_type": d.index_type,
                "type_url": getattr(d, "type_url", None), "num_indexed_rows": indexed,
                "num_unindexed_rows": max(total - indexed, 0),
                "num_segments": len(segments),
                "index_version": getattr(segments[0], "index_version", None) if segments else None,
            })
        names = [o["index_name"] for o in out]
        token = _paginate(names, req.get("page_token"), req.get("limit"))
        by_name = {o["index_name"]: o for o in out}
        return {"indexes": [by_name[n] for n in names], "page_token": token if names else None}

    def describe_table_index_stats(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("describe_table_index_stats")
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, req.get("version"), "describe_table_index_stats", uri)
        name = req.get("index_name")
        if name is None:
            raise ns_error("InvalidInput", "Index name is required for describe_table_index_stats")
        try:
            stats = ds.stats.index_stats(name)
        except Exception as exc:  # noqa: BLE001
            raise ns_error("TableIndexNotFound", f"Failed to describe index statistics for '{name}' on table "
                                                 f"'{uri}': {exc}") from None
        return {"distance_type": stats.get("distance_type"), "index_type": stats.get("index_type"),
                "num_indexed_rows": stats.get("num_indexed_rows"),
                "num_unindexed_rows": stats.get("num_unindexed_rows"), "num_indices": stats.get("num_indices")}

    def drop_table_index(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("drop_table_index")
        path, uri = self._resolve(req.get("id"))
        name = req.get("index_name")
        if name is None:
            raise ns_error("InvalidInput", "Index name is required for drop_table_index")
        ds = self._open(path, None, "drop_table_index", uri)
        try:
            ds.drop_index(name)
        except Exception as exc:  # noqa: BLE001
            raise ns_error("TableIndexNotFound", f"Failed to drop index '{name}' from table '{uri}': {exc}") from None
        return {"transaction_id": _transaction_uuid(ds)}

    # ── table maintenance ─────────────────────────────────────────────────────────────────────────

    def restore_table(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        version = int(req.get("version", 0))
        if version < 0:
            raise ValueError(f"Table version for restore_table must be non-negative, got {version}")
        self._check_branch(req)
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, None, "restore_table", uri)
        ds = ds.checkout_version(version)
        ds.restore()
        return {"transaction_id": _transaction_uuid(ds)}

    def update_table_schema_metadata(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, None, "update_table_schema_metadata", uri)
        updated = ds.update_schema_metadata(dict(req.get("metadata") or {}))
        return {"metadata": dict(updated), "transaction_id": _transaction_uuid(ds)}

    def get_table_stats(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, None, "get_table_stats", uri)
        data_stats = ds.stats.data_stats()
        total_bytes = sum(int(f.bytes_on_disk) for f in data_stats.fields)
        counts = sorted(f.physical_rows for f in ds.get_fragments())
        num_rows = sum(counts)
        if counts:
            n = len(counts)
            pct = lambda p: counts[int((n - 1) * p)]  # noqa: E731
            lengths = {"min": counts[0], "max": counts[-1], "mean": num_rows // n, "p25": pct(0.25),
                       "p50": pct(0.5), "p75": pct(0.75), "p99": pct(0.99)}
        else:
            lengths = {k: 0 for k in ("min", "max", "mean", "p25", "p50", "p75", "p99")}
        return {"total_bytes": total_bytes, "num_rows": num_rows, "num_indices": len(ds.list_indices()),
                "fragment_stats": {"num_fragments": len(counts),
                                   "num_small_fragments": sum(1 for c in counts if c < 1024 * 1024),
                                   "lengths": lengths}}

    def _alter(self, req: Dict[str, Any], fn, operation: str) -> Dict[str, Any]:
        table_id = req.get("id")
        if table_id is None:
            raise ValueError("Table ID is required")
        if not table_id:
            raise ValueError("Table ID cannot be empty")
        path, uri = self._resolve(table_id)
        ds = self._open(path)
        self._mutate(lambda: fn(ds), operation, uri)
        return {"version": ds.version}

    def alter_table_add_columns(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        columns = {}
        for col in req.get("new_columns") or []:
            if col.get("expression") is None:
                raise ValueError(f"Expression is required for new column '{col.get('name')}'")
            columns[col["name"]] = col["expression"]
        return self._alter(req, lambda ds: ds.add_columns(columns), "add_columns")

    def alter_table_alter_columns(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        alterations = []
        for entry in req.get("alterations") or []:
            alteration = {"path": entry["path"]}
            if entry.get("rename") is not None:
                alteration["name"] = entry["rename"]
            if entry.get("nullable") is not None:
                alteration["nullable"] = entry["nullable"]
            if entry.get("data_type") is not None:
                alteration["data_type"] = _arrow_type_from_json(entry["data_type"])
            alterations.append(alteration)
        return self._alter(req, lambda ds: ds.alter_columns(*alterations), "alter_columns")

    def alter_table_drop_columns(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        return self._alter(req, lambda ds: ds.drop_columns(list(req.get("columns") or [])), "drop_columns")

    # ── tags ──────────────────────────────────────────────────────────────────────────────────────

    def _tag_call(self, req: Dict[str, Any], operation: str, fn, check_version: bool = False):
        self._record(operation)
        tag = req.get("tag") or ""
        if not tag:
            raise ns_error("InvalidInput", f"tag name must not be empty for {operation}")
        if check_version and int(req.get("version") or 0) <= 0:
            raise ns_error("InvalidInput", f"tag version must be a positive integer, got {req.get('version')} "
                                           f"for {operation}")
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, None, operation, uri)
        try:
            return fn(ds, tag)
        except Exception as exc:  # noqa: BLE001
            text = str(exc)
            low = text.lower()
            if "not found" in low and "version" in low and "tag" not in low.split("version")[0]:
                raise ns_error("TableVersionNotFound", f"version referenced by tag '{tag}' not found for table at "
                                                       f"'{uri}': {text}") from None
            if "not found" in low or "does not exist" in low:
                raise ns_error("TableTagNotFound", f"tag '{tag}' for table at '{uri}'") from None
            if "already exists" in low or "conflict" in low:
                raise ns_error("TableTagAlreadyExists", f"tag '{tag}' for table at '{uri}'") from None
            if "invalid" in low:
                raise ns_error("InvalidInput", f"invalid tag '{tag}': {text}") from None
            raise ns_error("Internal", f"tag operation failed for tag '{tag}' on table at '{uri}': {text}") from None

    def list_table_tags(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("list_table_tags")
        path, uri = self._resolve(req.get("id"))
        ds = self._open(path, None, "list_table_tags", uri)
        tags = {name: {"version": int(t["version"]), "manifest_size": int(t.get("manifest_size") or 0),
                       "branch": t.get("branch")}
                for name, t in ds.tags.list().items()}
        return {"tags": tags, "page_token": None}

    def get_table_tag_version(self, request) -> Dict[str, Any]:
        req = _as_dict(request)

        def get(ds, tag):
            for name, t in ds.tags.list().items():
                if name == tag:
                    return {"version": int(t["version"]), "branch": t.get("branch")}
            raise ValueError(f"Ref not found error: tag {tag} does not exist")

        return self._tag_call(req, "get_table_tag_version", get)

    def create_table_tag(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._tag_call(req, "create_table_tag", lambda ds, tag: ds.tags.create(tag, int(req["version"])), True)
        return {"transaction_id": None}

    def delete_table_tag(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._tag_call(req, "delete_table_tag", lambda ds, tag: ds.tags.delete(tag))
        return {"transaction_id": None}

    def update_table_tag(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._tag_call(req, "update_table_tag", lambda ds, tag: ds.tags.update(tag, int(req["version"])), True)
        return {"transaction_id": None}

    # ── transactions ──────────────────────────────────────────────────────────────────────────────

    def _find_transaction(self, ds, txn_id: str):
        if txn_id.isdigit():
            txn = ds.read_transaction(int(txn_id))
            if txn is None:
                raise ns_error("TransactionNotFound", f"transaction {txn_id}")
            return int(txn_id), txn
        for version in range(ds.latest_version, 0, -1):
            txn = ds.read_transaction(version)
            if txn is not None and getattr(txn, "uuid", None) == txn_id:
                return version, txn
        raise ns_error("TransactionNotFound", f"transaction {txn_id}")

    def describe_transaction(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("describe_transaction")
        txn_id = req.get("id") or []
        if len(txn_id) < 2:
            raise ns_error("InvalidInput", "describe_transaction requires the table id followed by the transaction id")
        path, uri = self._resolve(txn_id[:-1])
        ds = self._open(path, None, "describe_transaction", uri)
        version, txn = self._find_transaction(ds, txn_id[-1])
        return _transaction_response(version, txn, self._load_alteration(path, txn))

    def _alteration_path(self, path: str, txn) -> str:
        return os.path.join(path, "_transactions", f"{txn.uuid}.alteration.json")

    def _load_alteration(self, path: str, txn) -> Optional[Dict[str, Any]]:
        try:
            with open(self._alteration_path(path, txn), "rb") as f:
                return json.loads(f.read())
        except FileNotFoundError:
            return None

    def alter_transaction(self, request) -> Dict[str, Any]:
        req = _as_dict(request)
        self._record("alter_transaction")
        txn_id = req.get("id") or []
        if len(txn_id) < 2:
            raise ns_error("InvalidInput", "alter_transaction requires the table id followed by the transaction id")
        path, uri = self._resolve(txn_id[:-1])
        ds = self._open(path, None, "alter_transaction", uri)
        version, txn = self._find_transaction(ds, txn_id[-1])
        alteration = self._load_alteration(path, txn) or {"status": None, "properties": {}, "removed_properties": []}
        for action in req.get("actions") or []:
            if action.get("set_status_action"):
                alteration["status"] = action["set_status_action"].get("status")
            if action.get("set_property_action"):
                prop = action["set_property_action"]
                alteration["properties"][prop["key"]] = prop["value"]
                alteration["removed_properties"] = [k for k in alteration["removed_properties"] if k != prop["key"]]
            if action.get("unset_property_action"):
                key = action["unset_property_action"]["key"]
                alteration["properties"].pop(key, None)
                if key not in alteration["removed_properties"]:
                    alteration["removed_properties"].append(key)
        target = self._alteration_path(path, txn)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        tmp = f"{target}.{uuid.uuid4().hex}.tmp"
        with open(tmp, "w") as f:
            json.dump(alteration, f)
        os.replace(tmp, target)
        return _transaction_response(version, txn, alteration)

    # ── branches ──────────────────────────────────────────────────────────────────────────────────

    def create_table_branch(self, request) -> Dict[str, Any]:
        raise ns_error("Unsupported", "table branches (nanolance does not implement them)")

    def list_table_branches(self, request) -> Dict[str, Any]:
        raise ns_error("Unsupported", "table branches (nanolance does not implement them)")

    def delete_table_branch(self, request) -> Dict[str, Any]:
        raise ns_error("Unsupported", "table branches (nanolance does not implement them)")

    def __getattr__(self, name: str):
        if name.startswith("_"):
            raise AttributeError(name)

        def unsupported_operation(*args, **kwargs):
            raise ns_error("Unsupported", f"{name} is not supported by DirectoryNamespace")

        return unsupported_operation


_SCALAR_INDEX_TYPES = {"BTREE", "BITMAP", "LABEL_LIST", "INVERTED", "NGRAM", "ZONEMAP", "BLOOMFILTER", "RTREE"}


def _parse_index_type(index_type: str) -> str:
    aliases = {
        "SCALAR": "BTREE", "BTREE": "BTREE", "BITMAP": "BITMAP", "LABEL_LIST": "LABEL_LIST",
        "LABELLIST": "LABEL_LIST", "INVERTED": "INVERTED", "FTS": "INVERTED", "NGRAM": "NGRAM",
        "ZONEMAP": "ZONEMAP", "ZONE_MAP": "ZONEMAP", "BLOOMFILTER": "BLOOMFILTER", "BLOOM_FILTER": "BLOOMFILTER",
        "RTREE": "RTREE", "R_TREE": "RTREE", "VECTOR": "IVF_PQ", "IVF_PQ": "IVF_PQ", "IVF_FLAT": "IVF_FLAT",
        "IVF_SQ": "IVF_SQ", "IVF_RQ": "IVF_RQ", "IVF_HNSW_FLAT": "IVF_HNSW_FLAT", "IVF_HNSW_SQ": "IVF_HNSW_SQ",
        "IVF_HNSW_PQ": "IVF_HNSW_PQ",
    }
    key = index_type.strip().upper()
    if key not in aliases:
        raise ns_error("InvalidInput", f"Unsupported index_type '{key}'")
    return aliases[key]


def _parse_metric(distance_type: Optional[str]) -> str:
    text = (distance_type or "l2").lower()
    metrics = {"l2": "l2", "euclidean": "l2", "cosine": "cosine", "dot": "dot", "inner_product": "dot",
               "hamming": "hamming"}
    if text not in metrics:
        raise ns_error("InvalidInput", f"Unsupported distance_type '{distance_type}' for vector index: "
                                       f"Invalid user input: Metric type '{distance_type}' is not supported")
    return metrics[text]


def _transaction_uuid(ds) -> Optional[str]:
    try:
        ds.checkout_latest()
        txn = ds.read_transaction()
        return getattr(txn, "uuid", None) if txn is not None else None
    except Exception:  # noqa: BLE001
        return None


def _version_response(version: int, final: str) -> Dict[str, Any]:
    stat = os.stat(final)
    return {"transaction_id": None, "version": {
        "version": version, "manifest_path": _store_path(final), "manifest_size": stat.st_size,
        "e_tag": _e_tag(stat), "timestamp_millis": None, "metadata": None}}


def _e_tag(stat) -> str:
    """object_store's local e-tag: inode, mtime in microseconds and size, in hex."""
    mtime = int(stat.st_mtime_ns // 1000)
    return f"{stat.st_ino:x}-{mtime:x}-{stat.st_size:x}"


def _transaction_response(version: int, txn, alteration: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    from nanolance.lance._transactions import operation_name

    properties = dict(getattr(txn, "transaction_properties", None) or {})
    status = "SUCCEEDED"
    if alteration:
        for key in alteration.get("removed_properties") or []:
            properties.pop(key, None)
        properties.update(alteration.get("properties") or {})
        if alteration.get("status"):
            status = alteration["status"]
    operation = txn.operation
    name = operation_name(operation)
    if name == "CreateIndex" and not getattr(operation, "new_indices", None) and getattr(operation,
                                                                                          "removed_indices", None):
        name = "DropIndex"
    properties.update(uuid=str(txn.uuid), version=str(version), read_version=str(txn.read_version), operation=name)
    return {"status": status, "properties": properties}


def _scanner_args(query: Dict[str, Any]) -> Dict[str, Any]:
    """Scanner arguments for a query's filter, columns, vector search, limit and offset."""
    args: Dict[str, Any] = {}
    if query.get("prefilter") is not None:
        args["prefilter"] = bool(query["prefilter"])
    if query.get("filter"):
        args["filter"] = query["filter"]
    columns = query.get("columns") or {}
    if isinstance(columns, dict):
        if columns.get("column_names"):
            args["columns"] = list(columns["column_names"])
        elif columns.get("column_aliases"):
            args["columns"] = dict(columns["column_aliases"])
    elif isinstance(columns, list) and columns:
        args["columns"] = list(columns)
    vector = query.get("vector") or {}
    single = vector.get("single_vector") if isinstance(vector, dict) else vector
    multi = vector.get("multi_vector") if isinstance(vector, dict) else None
    q = single if single else (multi[0] if multi and multi[0] else None)
    k = int(query.get("k") or 0)
    if q:
        nearest: Dict[str, Any] = {"column": query.get("vector_column") or "vector", "q": list(q), "k": k if k > 0 else 10}
        if query.get("distance_type"):
            nearest["metric"] = _parse_metric(query["distance_type"])
        if query.get("nprobes") is not None:
            nearest["minimum_nprobes"] = int(query["nprobes"])
        if query.get("ef") is not None:
            nearest["ef"] = int(query["ef"])
        if query.get("refine_factor") is not None:
            nearest["refine_factor"] = int(query["refine_factor"])
        if query.get("bypass_vector_index") is not None:
            nearest["use_index"] = not bool(query["bypass_vector_index"])
        if query.get("lower_bound") is not None or query.get("upper_bound") is not None:
            nearest["distance_range"] = (query.get("lower_bound"), query.get("upper_bound"))
        args["nearest"] = nearest
        if query.get("fast_search"):
            args["fast_search"] = True
        if query.get("offset"):
            args["offset"] = int(query["offset"])
    else:
        if k > 0:
            args["limit"] = k
        if query.get("offset") is not None:
            args["offset"] = int(query["offset"])
    fts = query.get("full_text_query")
    if fts:
        if fts.get("string_query"):
            sq = fts["string_query"]
            args["full_text_query"] = sq["query"] if not sq.get("columns") else {"query": sq["query"],
                                                                                  "columns": list(sq["columns"])}
        elif fts.get("structured_query"):
            raise ns_error("Unsupported", "structured full-text queries through the namespace API")
    if query.get("with_row_id"):
        args["with_row_id"] = True
    return args


def _run_query(ds, query: Dict[str, Any], apply_offset_without_vector: bool) -> pa.Table:
    return ds.scanner(**_scanner_args(query)).to_table()


def _arrow_type_from_json(value) -> pa.DataType:
    text = value if isinstance(value, str) else (value.get("type") if isinstance(value, dict) else str(value))
    names = {"null": pa.null(), "bool": pa.bool_(), "boolean": pa.bool_(), "int8": pa.int8(), "uint8": pa.uint8(),
             "int16": pa.int16(), "uint16": pa.uint16(), "int32": pa.int32(), "uint32": pa.uint32(),
             "int64": pa.int64(), "uint64": pa.uint64(), "float16": pa.float16(), "float32": pa.float32(),
             "float64": pa.float64(), "utf8": pa.utf8(), "string": pa.utf8(), "large_utf8": pa.large_utf8(),
             "binary": pa.binary(), "large_binary": pa.large_binary(), "date32": pa.date32(), "date64": pa.date64()}
    if text not in names:
        raise ValueError(f"Failed to parse data_type '{text}'")
    return names[text]


def _debug_list(parts: List[str]) -> str:
    return "[" + ", ".join(json.dumps(p) for p in parts) + "]"


def _debug_opt_list(parts: Optional[List[str]]) -> str:
    return "None" if parts is None else f"Some({_debug_list(parts)})"
