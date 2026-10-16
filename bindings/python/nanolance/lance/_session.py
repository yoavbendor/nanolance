"""``lance.Session``: per-session accounting and byte budgets over nanolance's process-wide caches.

nanolance's C++ caches (the index caches, the data-file metadata cache, the take cache) are process-wide
statics that cannot be split per session. A Session therefore keeps its own ledger of what the datasets
opened or read through it loaded: each manifest (by dataset and version), each data file's footer and
column metadata (by path), and each vector or full-text index read (by index UUID). An entry is counted
once per session, however many datasets share the session. Sizes are estimates of those entries on disk
(manifest and index sizes, the footer bytes of a data file), not the C++ caches' own accounting, so they
are per-session estimates over shared caches; ``size_bytes()`` adds a fixed 152 bytes of struct overhead,
as pylance's does. A budget admits an entry only while the ledger plus the entry fits in it.
"""

from __future__ import annotations

import os
import re
import struct
import threading
import uuid
from collections import OrderedDict
from typing import Any, Callable, Optional, Union
from urllib.parse import parse_qs, urlsplit

STRUCT_OVERHEAD_BYTES = 152
DEFAULT_CACHE_BYTES = 1 << 30
# Estimate for an entry that cannot be measured (a remote URI, or a file that cannot be read).
UNMEASURED_ENTRY_BYTES = 1024
_MAX_U64 = (1 << 64) - 1
_FOOTER_BYTES = 40
_FOOTER_MAGIC = b"LANC"


def _budget(value: Optional[int], name: str) -> int:
    if value is None:
        return DEFAULT_CACHE_BYTES
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"argument '{name}': '{type(value).__name__}' object cannot be interpreted as an integer")
    if value < 0:
        raise OverflowError("can't convert negative int to unsigned")
    return value


def _capacity(text: Optional[str], field: str) -> int:
    prefix = f"{field}: Invalid user input: moka cache backend: "
    if text is None:
        raise ValueError(prefix + "capacity is required; use moka://?capacity=<bytes>")
    if text == "":
        raise ValueError(prefix + "capacity must not be empty")
    if not re.fullmatch(r"\+?[0-9]+", text):
        raise ValueError(prefix + f'cannot parse capacity "{text}": invalid digit found in string')
    value = int(text)
    if value > _MAX_U64:
        raise ValueError(prefix + f'cannot parse capacity "{text}": number too large to fit in target type')
    return value


def _uri_capacity(uri: str, field: str) -> int:
    if ":" not in uri:
        raise ValueError(f'{field}: Invalid user input: cache backend uri "{uri}" is missing \':\'')
    parts = urlsplit(uri)
    if parts.scheme.lower() != "moka":
        raise ValueError(f'{field}: Invalid user input: unknown cache backend kind: "{parts.scheme}"')
    values = parse_qs(parts.query, keep_blank_values=True).get("capacity")
    return _capacity(values[0] if values else None, field)


def _dict_capacity(config: dict, field: str) -> int:
    for key in config:
        if key not in ("kind", "options"):
            raise ValueError(f"{field}: unknown dict key \"{key}\"; expected 'kind' or 'options'")
    if "kind" not in config:
        raise ValueError(f"{field}: dict must contain a 'kind' key")
    kind = config["kind"]
    if not isinstance(kind, str):
        raise ValueError(f"{field}: 'kind' must be a string")
    options = config.get("options") or {}
    if not isinstance(options, dict) or not all(isinstance(k, str) for k in options):
        raise ValueError(f"{field}: 'options' must be a dict[str, str]")
    if not all(isinstance(v, str) for v in options.values()):
        raise ValueError(f"{field}: 'options' values must be strings")
    if kind.lower() != "moka":
        raise ValueError(f'{field}: Invalid user input: unknown cache backend kind: "{kind}"')
    return _capacity(options.get("capacity"), field)


def _backend_capacity(backend: Union[str, dict], field: str) -> int:
    """The capacity (bytes) a cache backend URI or dict names, validated as pylance does."""
    if isinstance(backend, str):
        return _uri_capacity(backend, field)
    if isinstance(backend, dict):
        return _dict_capacity(backend, field)
    raise ValueError(f"{field}: expected str (URI) or dict with 'kind'/'options' keys, got {type(backend).__name__}")


class _Ledger:
    """Entries of one cache kind, with the budget that admits them."""

    def __init__(self, budget: int) -> None:
        self.budget = budget
        self.entries: OrderedDict = OrderedDict()

    @property
    def used(self) -> int:
        return sum(self.entries.values())

    def admit(self, key: Any, nbytes: Callable[[], int]) -> None:
        if key in self.entries or self.budget == 0:
            return
        size = nbytes()
        if self.used + size <= self.budget:
            self.entries[key] = size


class Session:
    """Shares cache budgets and accounting between datasets, like ``lance.Session``.

    ``index_cache_size_bytes`` / ``metadata_cache_size_bytes`` bound the entries the session records;
    ``index_cache_backend`` / ``metadata_cache_backend`` (a ``moka://?capacity=N`` URI or a dict) give
    the budget instead. ``store_registry`` is accepted for compatibility and not used.
    """

    def __init__(self, index_cache_size_bytes: Optional[int] = None, metadata_cache_size_bytes: Optional[int] = None,
                 index_cache_backend=None, metadata_cache_backend=None, store_registry=None) -> None:
        if index_cache_size_bytes is not None and index_cache_backend is not None:
            raise ValueError("index_cache_size_bytes and index_cache_backend are mutually exclusive; "
                             "set one or the other")
        if metadata_cache_size_bytes is not None and metadata_cache_backend is not None:
            raise ValueError("metadata_cache_size_bytes and metadata_cache_backend are mutually exclusive; "
                             "set one or the other")
        index_budget = (_backend_capacity(index_cache_backend, "index_cache_backend")
                        if index_cache_backend is not None else _budget(index_cache_size_bytes,
                                                                        "index_cache_size_bytes"))
        metadata_budget = (_backend_capacity(metadata_cache_backend, "metadata_cache_backend")
                           if metadata_cache_backend is not None else _budget(metadata_cache_size_bytes,
                                                                              "metadata_cache_size_bytes"))
        self._lock = threading.Lock()
        self._index = _Ledger(index_budget)
        self._metadata = _Ledger(metadata_budget)

    def size_bytes(self) -> int:
        """The estimated size of what this session holds: its entries plus the struct overhead."""
        with self._lock:
            return STRUCT_OVERHEAD_BYTES + self._metadata.used + self._index.used

    def index_cache_size_bytes(self) -> int:
        with self._lock:
            return self._index.used

    def metadata_cache_size_bytes(self) -> int:
        with self._lock:
            return self._metadata.used

    def is_same_as(self, other) -> bool:
        if not isinstance(other, Session):
            raise TypeError(f"argument 'other': '{type(other).__name__}' is not an instance of 'Session'")
        return self is other

    def __repr__(self) -> str:
        return (f"Session(index_cache_size_bytes={self.index_cache_size_bytes()}, "
                f"metadata_cache_size_bytes={self.metadata_cache_size_bytes()})")

    # Internal: the budgets, and recording under the lock.

    def _index_budget(self) -> int:
        return self._index.budget

    def _record(self, cache: str, key: Any, nbytes: Callable[[], int]) -> None:
        ledger = self._index if cache == "index" else self._metadata
        with self._lock:
            ledger.admit(key, nbytes)


def _local_dir(uri: str) -> Optional[str]:
    """The dataset directory of a local URI, or None for a remote one."""
    text = str(uri)
    if "://" in text:
        return None
    return os.path.abspath(text)


def _footer_bytes(path: str) -> int:
    """Bytes of a v2 data file from its column metadata to the end: the footer, descriptor and column
    metadata a session keeps for the file. Unmeasured (a fixed estimate) when the file is not readable."""
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            f.seek(max(0, size - _FOOTER_BYTES))
            tail = f.read(_FOOTER_BYTES)
    except OSError:
        return UNMEASURED_ENTRY_BYTES
    if len(tail) != _FOOTER_BYTES or tail[36:] != _FOOTER_MAGIC:
        return UNMEASURED_ENTRY_BYTES
    column_meta_start = struct.unpack_from("<Q", tail, 0)[0]
    return max(1, size - column_meta_start)


def _manifest_bytes(base: Optional[str], version: int) -> int:
    if base is None:
        return UNMEASURED_ENTRY_BYTES
    # Lance names a manifest by its version counted down from u64::MAX.
    for name in (f"{_MAX_U64 - version}.manifest", f"{version}.manifest"):
        try:
            return os.path.getsize(os.path.join(base, "_versions", name))
        except OSError:
            continue
    return UNMEASURED_ENTRY_BYTES


def _index_bytes(base: Optional[str], index_uuid: str) -> int:
    if base is None:
        return UNMEASURED_ENTRY_BYTES
    total = 0
    for root, _, files in os.walk(os.path.join(base, "_indices", index_uuid)):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(root, name))
            except OSError:
                pass
    return total or UNMEASURED_ENTRY_BYTES


def note_manifest(ds) -> None:
    """A dataset opened at a version loaded that version's manifest."""
    base = _local_dir(ds.uri)
    ds._session._record("metadata", ("manifest", ds.uri, ds.version),
                        lambda: _manifest_bytes(base, ds.version))


def note_data_files(ds) -> None:
    """A read of the dataset loaded the footers of the data files of its fragments."""
    session = ds._session
    if session._metadata.budget == 0:
        return
    base = _local_dir(ds.uri)
    for fragment in ds._info.get("fragments", []):
        for data_file in fragment.get("files", []):
            if base is None or data_file.get("base_id") is not None:
                key = ("data", ds.uri, data_file.get("base_id"), data_file["path"])
                session._record("metadata", key, lambda: UNMEASURED_ENTRY_BYTES)
            else:
                path = os.path.join(base, "data", data_file["path"])
                session._record("metadata", ("data", path), lambda path=path: _footer_bytes(path))


def note_indexes(ds, column: Optional[str] = None, kind: Optional[str] = None) -> None:
    """A search read the indexes of the dataset on ``column`` (or, with no column, those of ``kind``)."""
    session = ds._session
    if session._index_budget() == 0:
        return
    base = _local_dir(ds.uri)
    for index in ds.list_indices():
        if column is not None and column not in index["fields"]:
            continue
        if kind is not None and kind not in str(index["type"]).lower():
            continue
        index_uuid = str(index["uuid"])
        session._record("index", ("index", ds.uri, index_uuid), lambda u=index_uuid: _index_bytes(base, u))


def session_for_write(dataset_uri, session) -> None:
    """``write_fragments(..., session=)``: a dataset destination uses its own session (pylance rejects any
    other); a path loads the destination's manifest through ``session``."""
    if session is None:
        return
    from nanolance.lance.dataset import LanceDataset, _exists, _path_of

    if isinstance(dataset_uri, LanceDataset):
        if not dataset_uri.session().is_same_as(session):
            raise ValueError("The provided session is not the destination dataset's own session. "
                             "Please pass the dataset's session or omit the 'session' parameter.")
        return
    path = _path_of(dataset_uri)
    if _exists(path):
        # Schema inference reads the destination's manifest and the footers of its data files.
        note_data_files(LanceDataset(path, session=session))
