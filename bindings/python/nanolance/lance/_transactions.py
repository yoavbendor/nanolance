# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor

"""Transactions as pylance presents them: ``LanceOperation.*`` and ``Transaction``, read from the
``_transactions/*.txn`` file a version's manifest names (lance.table.Transaction, protobuf).

Hand-built transactions go the other way: an operation is encoded here (``encode_operation``) as
Lance encodes it, checked against the transactions committed since its read version as Lance checks
them (``conflict_with``), and committed by the native library (dataset_transaction.hpp).
"""

from __future__ import annotations

import dataclasses
import struct
import uuid as _uuid
from abc import ABC
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Dict, Iterator, List, Optional, Set, Tuple

from nanolance.lance.fragment import (DataFile, DeletionFile, FragmentMetadata, RowDatasetVersionMeta,
                                      RowIdMeta)

__all__ = ["Index", "IndexFile", "LanceOperation", "Transaction", "decode_transaction"]


@dataclass
class IndexFile:
    path: str
    size_bytes: int


@dataclass
class Index:
    """An index segment, as a transaction records it."""

    uuid: str
    name: str
    fields: List[int]
    dataset_version: int
    fragment_ids: Set[int]
    index_version: int
    created_at: Optional[datetime] = None
    base_id: Optional[int] = None
    files: Optional[List[IndexFile]] = None
    index_details: Optional[Tuple[str, bytes]] = None
    covering_fields: List[int] = dataclasses.field(default_factory=list)


def _validate_fragments(fragments) -> None:
    if not isinstance(fragments, list):
        raise TypeError(f"fragments must be list[FragmentMetadata], got {type(fragments)}")
    if fragments and not all(isinstance(f, FragmentMetadata) for f in fragments):
        raise TypeError(f"fragments must be list[FragmentMetadata], got {type(fragments[0])}")


def _as_lance_schema(schema):
    import pyarrow as pa

    from nanolance.lance.schema import LanceSchema

    return LanceSchema.from_pyarrow(schema) if isinstance(schema, pa.Schema) else schema


class LanceOperation:
    _validate_fragments = staticmethod(_validate_fragments)

    class BaseOperation(ABC):
        """An operation a transaction applies."""

    @dataclass
    class Overwrite(BaseOperation):
        new_schema: Any
        fragments: List[FragmentMetadata]
        initial_bases: Optional[list] = None

        def __post_init__(self):
            self.new_schema = _as_lance_schema(self.new_schema)
            _validate_fragments(self.fragments)

    @dataclass
    class Append(BaseOperation):
        fragments: List[FragmentMetadata]

        def __post_init__(self):
            _validate_fragments(self.fragments)

    @dataclass
    class Delete(BaseOperation):
        updated_fragments: List[FragmentMetadata]
        deleted_fragment_ids: List[int]
        predicate: str

        def __post_init__(self):
            _validate_fragments(self.updated_fragments)

    @dataclass
    class Update(BaseOperation):
        removed_fragment_ids: List[int] = dataclasses.field(default_factory=list)
        updated_fragments: List[FragmentMetadata] = dataclasses.field(default_factory=list)
        new_fragments: List[FragmentMetadata] = dataclasses.field(default_factory=list)
        fields_modified: List[int] = dataclasses.field(default_factory=list)
        fields_for_preserving_frag_bitmap: List[int] = dataclasses.field(default_factory=list)
        update_mode: str = ""
        updated_fragment_offsets: Optional[Dict[int, bytes]] = None

        def __post_init__(self):
            _validate_fragments(self.updated_fragments)
            _validate_fragments(self.new_fragments)

    @dataclass
    class Merge(BaseOperation):
        fragments: List[FragmentMetadata]
        schema: Any
        preserves_nullability: bool = False

        def __post_init__(self):
            import pyarrow as pa

            if isinstance(self.schema, pa.Schema):
                import warnings

                warnings.warn("Passing a pyarrow.Schema to Merge is deprecated. Please use a LanceSchema instead.",
                              DeprecationWarning)
                self.schema = _as_lance_schema(self.schema)
            _validate_fragments(self.fragments)

    @dataclass
    class Restore(BaseOperation):
        version: int

    @dataclass
    class RewriteGroup:
        old_fragments: List[FragmentMetadata]
        new_fragments: List[FragmentMetadata]

    @dataclass
    class RewrittenIndex:
        old_id: str
        new_id: str
        new_details_type_url: str
        new_details_value: bytes
        new_index_version: int

    @dataclass
    class Rewrite(BaseOperation):
        groups: List["LanceOperation.RewriteGroup"]
        rewritten_indices: List["LanceOperation.RewrittenIndex"]

        def __post_init__(self):
            frags = [f for g in self.groups for f in g.old_fragments] + [f for g in self.groups for f in g.new_fragments]
            _validate_fragments(frags)

    @dataclass
    class CreateIndex(BaseOperation):
        new_indices: List[Index]
        removed_indices: List[Index]

    @dataclass
    class DataReplacementGroup:
        fragment_id: int
        new_file: DataFile

    @dataclass
    class DataReplacement(BaseOperation):
        replacements: List["LanceOperation.DataReplacementGroup"]

    @dataclass
    class DataOverlayFile:
        data_file: DataFile
        offsets: Any
        committed_version: Optional[int] = None

    @dataclass
    class DataOverlayGroup:
        fragment_id: int
        overlays: List["LanceOperation.DataOverlayFile"]

    @dataclass
    class DataOverlay(BaseOperation):
        groups: List["LanceOperation.DataOverlayGroup"]

    @dataclass
    class Project(BaseOperation):
        schema: Any
        preserves_nullability: bool = False

    @dataclass
    class UpdateMap:
        updates: Dict[str, Optional[str]]
        replace: bool = False

    @dataclass
    class UpdateConfig(BaseOperation):
        config_updates: Optional["LanceOperation.UpdateMap"] = None
        table_metadata_updates: Optional["LanceOperation.UpdateMap"] = None
        schema_metadata_updates: Optional["LanceOperation.UpdateMap"] = None
        field_metadata_updates: Optional[Dict[int, "LanceOperation.UpdateMap"]] = None


@dataclass
class Transaction:
    read_version: int
    operation: LanceOperation.BaseOperation
    uuid: str = dataclasses.field(default_factory=lambda: str(_uuid.uuid4()))
    transaction_properties: Optional[Dict[str, str]] = dataclasses.field(default_factory=dict)


# ── protobuf ─────────────────────────────────────────────────────────────────────────────────────


def _fields(data: bytes) -> Iterator[Tuple[int, int, Any]]:
    """(field number, wire type, value): an int for varint / fixed, bytes for length-delimited."""
    pos, end = 0, len(data)

    def varint():
        nonlocal pos
        shift = value = 0
        while True:
            if pos >= end:
                raise ValueError("truncated transaction")
            b = data[pos]
            pos += 1
            value |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return value

    while pos < end:
        key = varint()
        number, wire = key >> 3, key & 7
        if wire == 0:
            yield number, wire, varint()
        elif wire == 1:
            yield number, wire, struct.unpack_from("<Q", data, pos)[0]
            pos += 8
        elif wire == 2:
            n = varint()
            yield number, wire, data[pos:pos + n]
            pos += n
        elif wire == 5:
            yield number, wire, struct.unpack_from("<I", data, pos)[0]
            pos += 4
        else:
            raise ValueError(f"unsupported wire type {wire}")


def _varints(data: bytes) -> List[int]:
    out, pos = [], 0
    while pos < len(data):
        shift = value = 0
        while True:
            b = data[pos]
            pos += 1
            value |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
        out.append(value)
    return out


def _int32(v: int) -> int:
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def _ints(wire: int, value, signed: bool = False) -> List[int]:
    values = [value] if wire == 0 else _varints(value)
    return [_int32(v) for v in values] if signed else values


def _data_file(data: bytes) -> DataFile:
    f = DataFile(path="", fields=[], column_indices=[], file_major_version=0, file_minor_version=0)
    for number, wire, value in _fields(data):
        if number == 1:
            f._path = value.decode()
        elif number == 2:
            f.fields += _ints(wire, value, True)
        elif number == 3:
            f.column_indices += _ints(wire, value, True)
        elif number == 4:
            f.file_major_version = value
        elif number == 5:
            f.file_minor_version = value
        elif number == 6:
            f.file_size_bytes = value
        elif number == 7:
            f.base_id = value
    return f


def _fragment(data: bytes) -> FragmentMetadata:
    fragment = FragmentMetadata(id=0, files=[], physical_rows=0)
    for number, _, value in _fields(data):
        if number == 1:
            fragment.id = value
        elif number == 2:
            fragment.files.append(_data_file(value))
        elif number == 3:
            d = {n: v for n, _, v in _fields(value)}
            fragment.deletion_file = DeletionFile(d.get(2, 0), d.get(3, 0), "bitmap" if d.get(1, 0) == 1 else "array",
                                                  d.get(4, 0), d.get(7))
        elif number == 4:
            fragment.physical_rows = value
        elif number == 5:
            fragment.row_id_meta = RowIdMeta(bytes(value))
        elif number == 7:
            fragment.last_updated_at_version_meta = RowDatasetVersionMeta(bytes(value))
        elif number == 9:
            fragment.created_at_version_meta = RowDatasetVersionMeta(bytes(value))
    return fragment


def _fragment_id(data: bytes) -> int:
    for number, _, value in _fields(data):
        if number == 1:
            return value
    return 0


def _roaring(data: bytes) -> Set[int]:
    """A portable RoaringBitmap serialization's values."""
    out: Set[int] = set()
    if len(data) < 4:
        return out
    cookie = struct.unpack_from("<I", data, 0)[0]
    pos = 4
    if cookie & 0xFFFF == 12347:  # with run containers
        n = (cookie >> 16) + 1
        run_bytes = (n + 7) // 8
        runs = data[pos:pos + run_bytes]
        pos += run_bytes
        has_offsets = n >= 4
    elif cookie == 12346:
        n = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        runs = b"\x00" * ((n + 7) // 8)
        has_offsets = True
    else:
        return out
    headers = [struct.unpack_from("<HH", data, pos + 4 * k) for k in range(n)]
    pos += 4 * n
    if has_offsets:
        pos += 4 * n
    for k, (key, card_minus_one) in enumerate(headers):
        high = key << 16
        if runs[k // 8] >> (k % 8) & 1:
            count = struct.unpack_from("<H", data, pos)[0]
            pos += 2
            for _ in range(count):
                start, length = struct.unpack_from("<HH", data, pos)
                pos += 4
                out.update(high | v for v in range(start, start + length + 1))
        elif card_minus_one + 1 > 4096:
            words = struct.unpack_from("<1024Q", data, pos)
            pos += 8192
            for w, word in enumerate(words):
                while word:
                    low = word & -word
                    out.add(high | (w * 64 + low.bit_length() - 1))
                    word ^= low
        else:
            values = struct.unpack_from(f"<{card_minus_one + 1}H", data, pos)
            pos += 2 * (card_minus_one + 1)
            out.update(high | v for v in values)
    return out


def _uuid_of(data: bytes) -> str:
    for number, _, value in _fields(data):
        if number == 1 and len(value) == 16:
            return str(_uuid.UUID(bytes=bytes(value)))
    return ""


def _any(data: bytes) -> Tuple[str, bytes]:
    url, value = "", b""
    for number, _, v in _fields(data):
        if number == 1:
            url = v.decode()
        elif number == 2:
            value = bytes(v)
    return url, value


def _index(data: bytes) -> Index:
    index = Index(uuid="", name="", fields=[], dataset_version=0, fragment_ids=set(), index_version=0)
    for number, wire, value in _fields(data):
        if number == 1:
            index.uuid = _uuid_of(value)
        elif number == 2:
            index.fields += _ints(wire, value, True)
        elif number == 3:
            index.name = value.decode()
        elif number == 4:
            index.dataset_version = value
        elif number == 5:
            index.fragment_ids = _roaring(bytes(value))
        elif number == 6:
            index.index_details = _any(value)
        elif number == 7:
            index.index_version = _int32(value)
        elif number == 8:
            index.created_at = datetime.fromtimestamp(value / 1000, tz=timezone.utc)
        elif number == 9:
            index.base_id = value
        elif number == 10:
            path, size = "", 0
            for n, _, v in _fields(value):
                if n == 1:
                    path = v.decode()
                elif n == 2:
                    size = v
            index.files = (index.files or []) + [IndexFile(path, size)]
        elif number == 11:
            index.covering_fields += _ints(wire, value, True)
    return index


def _schema(field_protos: List[bytes], metadata: Dict[str, bytes]):
    from nanolance.lance.schema import LanceSchema

    return LanceSchema._from_messages([bytes(p) for p in field_protos],
                                      {k: v.decode(errors="replace") for k, v in metadata.items()})


def _map_entry(data: bytes) -> Tuple[Any, Any]:
    key = value = None
    for number, _, v in _fields(data):
        if number == 1:
            key = v
        elif number == 2:
            value = v
    return key, value


def _update_map(data: bytes) -> "LanceOperation.UpdateMap":
    updates: Dict[str, Optional[str]] = {}
    replace = False
    for number, _, value in _fields(data):
        if number == 1:
            key, v = _map_entry(value)
            updates[(key or b"").decode()] = None if v is None else v.decode()
        elif number == 2:
            replace = bool(value)
    return LanceOperation.UpdateMap(updates, replace)


def _operation(number: int, data: bytes) -> LanceOperation.BaseOperation:
    items = list(_fields(data))
    if number == 100:
        return LanceOperation.Append([_fragment(v) for n, _, v in items if n == 1])
    if number == 101:
        deleted: List[int] = []
        predicate = ""
        for n, wire, v in items:
            if n == 2:
                deleted += _ints(wire, v)
            elif n == 3:
                predicate = v.decode()
        return LanceOperation.Delete([_fragment(v) for n, _, v in items if n == 1], deleted, predicate)
    if number == 102:
        metadata = dict(_map_entry(v) for n, _, v in items if n == 3)
        schema = _schema([v for n, _, v in items if n == 2], {k.decode(): bytes(v or b"") for k, v in metadata.items()})
        return LanceOperation.Overwrite(schema, [_fragment(v) for n, _, v in items if n == 1])
    if number == 103:
        return LanceOperation.CreateIndex([_index(v) for n, _, v in items if n == 1],
                                          [_index(v) for n, _, v in items if n == 2])
    if number == 104:
        groups = []
        legacy_old = [_fragment(v) for n, _, v in items if n == 1]
        legacy_new = [_fragment(v) for n, _, v in items if n == 2]
        if legacy_old or legacy_new:
            groups.append(LanceOperation.RewriteGroup(legacy_old, legacy_new))
        for n, _, v in items:
            if n == 3:
                parts = list(_fields(v))
                groups.append(LanceOperation.RewriteGroup([_fragment(x) for m, _, x in parts if m == 1],
                                                          [_fragment(x) for m, _, x in parts if m == 2]))
        rewritten = []
        for n, _, v in items:
            if n == 4:
                old_id = new_id = ""
                url, details, version = "", b"", 0
                for m, _, x in _fields(v):
                    if m == 1:
                        old_id = _uuid_of(x)
                    elif m == 2:
                        new_id = _uuid_of(x)
                    elif m == 3:
                        url, details = _any(x)
                    elif m == 4:
                        version = x
                rewritten.append(LanceOperation.RewrittenIndex(old_id, new_id, url, details, version))
        return LanceOperation.Rewrite(groups, rewritten)
    if number == 105:
        metadata = dict(_map_entry(v) for n, _, v in items if n == 3)
        schema = _schema([v for n, _, v in items if n == 2], {k.decode(): bytes(v or b"") for k, v in metadata.items()})
        preserves = any(n == 4 and v for n, _, v in items)
        return LanceOperation.Merge([_fragment(v) for n, _, v in items if n == 1], schema, preserves)
    if number == 106:
        return LanceOperation.Restore(next((v for n, _, v in items if n == 1), 0))
    if number == 107:
        op = LanceOperation.BaseOperation.__new__(_ReserveFragments)
        op._num_fragments = next((v for n, _, v in items if n == 1), 0)
        return op
    if number == 108:
        op = LanceOperation.Update()
        offsets: Dict[int, bytes] = {}
        for n, wire, v in items:
            if n == 1:
                op.removed_fragment_ids += _ints(wire, v)
            elif n == 2:
                op.updated_fragments.append(_fragment(v))
            elif n == 3:
                op.new_fragments.append(_fragment(v))
            elif n == 4:
                op.fields_modified += _ints(wire, v)
            elif n == 6:
                op.fields_for_preserving_frag_bitmap += _ints(wire, v)
            elif n == 7:
                op.update_mode = "rewrite_columns" if v == 1 else "rewrite_rows"
            elif n == 10:
                key, value = _map_entry(v)
                offsets[int(key or 0)] = bytes(value or b"")
        if not op.update_mode:
            op.update_mode = "rewrite_rows"
        op.updated_fragment_offsets = offsets or None
        return op
    if number == 109:
        preserves = any(n == 2 and v for n, _, v in items)
        return LanceOperation.Project(_schema([v for n, _, v in items if n == 1], {}), preserves)
    if number == 110:
        op = LanceOperation.UpdateConfig()
        for n, _, v in items:
            if n == 6:
                op.config_updates = _update_map(v)
            elif n == 7:
                op.table_metadata_updates = _update_map(v)
            elif n == 8:
                op.schema_metadata_updates = _update_map(v)
            elif n == 9:
                key, value = _map_entry(v)
                if op.field_metadata_updates is None:
                    op.field_metadata_updates = {}
                op.field_metadata_updates[_int32(key or 0)] = _update_map(value or b"")
        return op
    if number == 111:
        groups = []
        for n, _, v in items:
            if n == 1:
                fragment_id, new_file = 0, None
                for m, _, x in _fields(v):
                    if m == 1:
                        fragment_id = x
                    elif m == 2:
                        new_file = _data_file(x)
                groups.append(LanceOperation.DataReplacementGroup(fragment_id, new_file))
        return LanceOperation.DataReplacement(groups)
    other = LanceOperation.BaseOperation.__new__(_OtherOperation)
    other._number = number
    return other


class _ReserveFragments(LanceOperation.BaseOperation):
    """ReserveFragments: pylance presents it as a bare BaseOperation."""

    __qualname__ = "BaseOperation"


_ReserveFragments.__name__ = "BaseOperation"


class _OtherOperation(LanceOperation.BaseOperation):
    __qualname__ = "BaseOperation"


_OtherOperation.__name__ = "BaseOperation"


def decode_transaction(data: bytes) -> Transaction:
    read_version, txn_uuid, properties = 0, "", {}
    operation: Optional[LanceOperation.BaseOperation] = None
    for number, _, value in _fields(data):
        if number == 1:
            read_version = value
        elif number == 2:
            txn_uuid = value.decode()
        elif number == 4:
            key, v = _map_entry(value)
            properties[(key or b"").decode()] = (v or b"").decode()
        elif 100 <= number < 200:
            operation = _operation(number, bytes(value))
    if operation is None:
        operation = _OtherOperation()
    return Transaction(read_version=read_version, operation=operation, uuid=txn_uuid,
                       transaction_properties=properties)


# ── encoding: a hand-built operation as Lance writes it ─────────────────────────────────────────


def _put_varint(out: bytearray, v: int) -> None:
    v &= (1 << 64) - 1  # a negative int32 (a tombstoned field id) is ten bytes, two's complement
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)


def _put_uint(out: bytearray, number: int, v: int) -> None:
    if v:
        _put_varint(out, number << 3)
        _put_varint(out, int(v))


def _put_bytes(out: bytearray, number: int, data: bytes) -> None:
    _put_varint(out, (number << 3) | 2)
    _put_varint(out, len(data))
    out += data


def _put_packed(out: bytearray, number: int, values) -> None:
    values = list(values)
    if values:
        packed = bytearray()
        for v in values:
            _put_varint(packed, int(v))
        _put_bytes(out, number, bytes(packed))


def _encode_data_file(f: DataFile) -> bytes:
    out = bytearray()
    if f._path:
        _put_bytes(out, 1, f._path.encode())
    _put_packed(out, 2, f.fields)
    _put_packed(out, 3, f.column_indices)
    _put_uint(out, 4, f.file_major_version)
    _put_uint(out, 5, f.file_minor_version)
    _put_uint(out, 6, f.file_size_bytes or 0)
    if f.base_id is not None:
        _put_varint(out, 7 << 3)
        _put_varint(out, int(f.base_id))
    return bytes(out)


def _encode_fragment(f: FragmentMetadata) -> bytes:
    from nanolance.lance._errors import unsupported

    if f.overlays:
        raise unsupported("data overlay files")
    out = bytearray()
    _put_uint(out, 1, f.id)
    for file in f.files:
        _put_bytes(out, 2, _encode_data_file(file))
    d = f.deletion_file
    if d is not None:
        deletion = bytearray()
        _put_uint(deletion, 1, 1 if d.file_type == "bitmap" else 0)
        _put_uint(deletion, 2, d.read_version)
        _put_uint(deletion, 3, d.id)
        _put_uint(deletion, 4, d.num_deleted_rows)
        if d.base_id is not None:
            _put_varint(deletion, 7 << 3)
            _put_varint(deletion, int(d.base_id))
        _put_bytes(out, 3, bytes(deletion))
    _put_uint(out, 4, f.physical_rows)
    if f.row_id_meta is not None:
        _put_bytes(out, 5, bytes(f.row_id_meta._inline))
    if f.last_updated_at_version_meta is not None:
        _put_bytes(out, 7, bytes(f.last_updated_at_version_meta._inline))
    if f.created_at_version_meta is not None:
        _put_bytes(out, 9, bytes(f.created_at_version_meta._inline))
    return bytes(out)


def _put_map_entry(out: bytearray, number: int, key: bytes, value: Optional[bytes]) -> None:
    entry = bytearray()
    _put_bytes(entry, 1, key)
    if value is not None:
        _put_bytes(entry, 2, value)
    _put_bytes(out, number, bytes(entry))


def _schema_messages(schema) -> Tuple[List[bytes], Dict[str, str]]:
    from nanolance.lance._errors import unsupported

    schema = _as_lance_schema(schema)
    messages = getattr(schema, "_messages", None)
    if messages is None:
        raise unsupported("a LanceSchema without its Lance fields")
    return list(messages), dict(schema._metadata)


def _check_roaring(data: bytes, fragment_id: int) -> None:
    """Refuse bytes that are not a portable RoaringBitmap, as Lance does before committing them."""
    def bad(why: str):
        raise ValueError(f"Invalid user input: updated_fragment_offsets for fragment {fragment_id} is not a "
                         f"valid portable RoaringBitmap ({why})")

    if len(data) < 8:
        bad("too short")
    cookie = struct.unpack_from("<I", data, 0)[0]
    if cookie == 12346:
        count, pos, runs = struct.unpack_from("<I", data, 4)[0], 8, b""
    elif cookie & 0xFFFF == 12347:
        count, pos = (cookie >> 16) + 1, 4
        runs = data[pos:pos + (count + 7) // 8]
        pos += (count + 7) // 8
    else:
        bad("unknown cookie")
    if count > 65536 or pos + 4 * count > len(data):
        bad("truncated header")
    try:
        _roaring(data)
    except (struct.error, IndexError, ValueError) as exc:
        bad(str(exc) or "truncated container")


def _encode_update_map(m: "LanceOperation.UpdateMap") -> bytes:
    out = bytearray()
    for key, value in m.updates.items():
        _put_map_entry(out, 1, str(key).encode(), None if value is None else str(value).encode())
    _put_uint(out, 2, 1 if m.replace else 0)
    return bytes(out)


def _uuid_message(text: str) -> bytes:
    out = bytearray()
    _put_bytes(out, 1, _uuid.UUID(str(text)).bytes)
    return bytes(out)


_OPERATION_FIELDS = {"Append": 100, "Delete": 101, "Overwrite": 102, "CreateIndex": 103, "Rewrite": 104,
                     "Merge": 105, "Restore": 106, "ReserveFragments": 107, "Update": 108, "Project": 109,
                     "UpdateConfig": 110, "DataReplacement": 111, "UpdateMemWalState": 112, "Clone": 113,
                     "UpdateBases": 114, "DataOverlay": 115}


def operation_name(op: LanceOperation.BaseOperation) -> str:
    """Lance's name of an operation ("Append", ...)."""
    if isinstance(op, _ReserveFragments):
        return "ReserveFragments"
    if isinstance(op, _OtherOperation):
        number = getattr(op, "_number", 0)
        return next((k for k, v in _OPERATION_FIELDS.items() if v == number), "Unknown")
    return type(op).__name__


def encode_operation(op: LanceOperation.BaseOperation) -> Tuple[int, bytes]:
    """The operation's field number in lance.table.Transaction and its message."""
    from nanolance.lance._errors import unsupported

    out = bytearray()
    L = LanceOperation
    if isinstance(op, L.Append):
        for f in op.fragments:
            _put_bytes(out, 1, _encode_fragment(f))
    elif isinstance(op, L.Delete):
        for f in op.updated_fragments:
            _put_bytes(out, 1, _encode_fragment(f))
        _put_packed(out, 2, op.deleted_fragment_ids)
        if op.predicate:
            _put_bytes(out, 3, str(op.predicate).encode())
    elif isinstance(op, L.Overwrite):
        if op.initial_bases:
            raise unsupported("Overwrite with initial_bases (multiple base paths)")
        for f in op.fragments:
            _put_bytes(out, 1, _encode_fragment(f))
        messages, metadata = _schema_messages(op.new_schema)
        for m in messages:
            _put_bytes(out, 2, m)
        for k, v in metadata.items():
            _put_map_entry(out, 3, k.encode(), v.encode())
    elif isinstance(op, L.Rewrite):
        for g in op.groups:
            group = bytearray()
            for f in g.old_fragments:
                _put_bytes(group, 1, _encode_fragment(f))
            for f in g.new_fragments:
                _put_bytes(group, 2, _encode_fragment(f))
            _put_bytes(out, 3, bytes(group))
        for ri in op.rewritten_indices:
            entry = bytearray()
            _put_bytes(entry, 1, _uuid_message(ri.old_id))
            _put_bytes(entry, 2, _uuid_message(ri.new_id))
            details = bytearray()
            if ri.new_details_type_url:
                _put_bytes(details, 1, ri.new_details_type_url.encode())
            if ri.new_details_value:
                _put_bytes(details, 2, bytes(ri.new_details_value))
            _put_bytes(entry, 3, bytes(details))
            _put_uint(entry, 4, ri.new_index_version)
            _put_bytes(out, 4, bytes(entry))
    elif isinstance(op, L.Merge):
        for f in op.fragments:
            _put_bytes(out, 1, _encode_fragment(f))
        messages, metadata = _schema_messages(op.schema)
        for m in messages:
            _put_bytes(out, 2, m)
        for k, v in metadata.items():
            _put_map_entry(out, 3, k.encode(), v.encode())
        _put_uint(out, 4, 1 if op.preserves_nullability else 0)
    elif isinstance(op, L.Restore):
        _put_uint(out, 1, op.version)
    elif isinstance(op, _ReserveFragments):
        _put_uint(out, 1, getattr(op, "_num_fragments", 0))
    elif isinstance(op, L.Update):
        _put_packed(out, 1, op.removed_fragment_ids)
        for f in op.updated_fragments:
            _put_bytes(out, 2, _encode_fragment(f))
        for f in op.new_fragments:
            _put_bytes(out, 3, _encode_fragment(f))
        _put_packed(out, 4, op.fields_modified)
        _put_packed(out, 6, op.fields_for_preserving_frag_bitmap)
        mode = (op.update_mode or "").lower()
        if mode not in ("", "rewrite_rows", "rewrite_columns"):
            raise ValueError(f"Invalid update_mode: {op.update_mode!r} (expected rewrite_rows or rewrite_columns)")
        _put_uint(out, 7, 1 if mode == "rewrite_columns" else 0)
        for fragment_id, offsets in sorted((op.updated_fragment_offsets or {}).items()):
            _check_roaring(bytes(offsets), fragment_id)
            entry = bytearray()
            _put_uint(entry, 1, int(fragment_id))
            _put_bytes(entry, 2, bytes(offsets))
            _put_bytes(out, 10, bytes(entry))
    elif isinstance(op, L.Project):
        messages, _ = _schema_messages(op.schema)
        for m in messages:
            _put_bytes(out, 1, m)
        _put_uint(out, 2, 1 if op.preserves_nullability else 0)
    elif isinstance(op, L.UpdateConfig):
        if op.config_updates is not None:
            _put_bytes(out, 6, _encode_update_map(op.config_updates))
        if op.table_metadata_updates is not None:
            _put_bytes(out, 7, _encode_update_map(op.table_metadata_updates))
        if op.schema_metadata_updates is not None:
            _put_bytes(out, 8, _encode_update_map(op.schema_metadata_updates))
        for field_id, m in sorted((op.field_metadata_updates or {}).items()):
            entry = bytearray()
            _put_uint(entry, 1, int(field_id))
            _put_bytes(entry, 2, _encode_update_map(m))
            _put_bytes(out, 9, bytes(entry))
    elif isinstance(op, L.DataReplacement):
        for g in op.replacements:
            group = bytearray()
            _put_uint(group, 1, g.fragment_id)
            _put_bytes(group, 2, _encode_data_file(g.new_file))
            _put_bytes(out, 1, bytes(group))
    else:
        raise unsupported(f"LanceDataset.commit of {operation_name(op)}")
    return _OPERATION_FIELDS[operation_name(op)], bytes(out)


# ── conflicts: Lance's rules for a transaction committed since the read version ─────────────────
#
# lance's io/commit/conflict_resolver.rs, for transactions built by hand (no affected-rows map: a
# concurrent change to the same fragments is a retryable conflict, the caller re-reads and retries).


def _touched(op) -> Set[int]:
    """The existing fragments an operation changes or removes."""
    L = LanceOperation
    if isinstance(op, L.Delete):
        return {f.id for f in op.updated_fragments} | set(op.deleted_fragment_ids)
    if isinstance(op, L.Update):
        return {f.id for f in op.updated_fragments} | set(op.removed_fragment_ids)
    if isinstance(op, L.Rewrite):
        return {f.id for g in op.groups for f in g.old_fragments}
    if isinstance(op, L.DataReplacement):
        return {g.fragment_id for g in op.replacements}
    return set()


def _may_alter_nullability(op) -> bool:
    return isinstance(op, (LanceOperation.Project, LanceOperation.Merge)) and not op.preserves_nullability


def _supplies_values(op) -> bool:
    L = LanceOperation
    return isinstance(op, (L.Append, L.Update, L.DataReplacement)) or operation_name(op) == "DataOverlay"


def _updates_schema_metadata(op) -> bool:
    if not isinstance(op, LanceOperation.UpdateConfig):
        return False
    m = op.schema_metadata_updates
    if m is not None and (m.replace or m.updates):
        return True
    return any(u.replace or u.updates for u in (op.field_metadata_updates or {}).values())


def _upsert_keys(op) -> Tuple[Set[str], Set[str]]:
    """(config keys set, config keys removed) by an Overwrite or UpdateConfig."""
    if isinstance(op, LanceOperation.UpdateConfig) and op.config_updates is not None:
        upserts = {k for k, v in op.config_updates.updates.items() if v is not None}
        deletes = {k for k, v in op.config_updates.updates.items() if v is None}
        return upserts, deletes
    return set(), set()


def conflict_with(ours, theirs) -> Optional[str]:
    """None when `ours` still applies after `theirs` was committed; else "retryable" (re-read and
    try again) or "incompatible"."""
    L = LanceOperation
    name = operation_name(theirs)
    if (_may_alter_nullability(ours) and _supplies_values(theirs)) or \
            (_supplies_values(ours) and _may_alter_nullability(theirs)):
        return "retryable"
    if (isinstance(ours, L.Merge) and _updates_schema_metadata(theirs)) or \
            (_updates_schema_metadata(ours) and isinstance(theirs, L.Merge)):
        return "retryable"
    if name == "UpdateMemWalState":
        return "incompatible"
    always_ok = {"Clone", "UpdateBases"}
    if isinstance(ours, L.Append):
        return "incompatible" if name in ("Overwrite", "Restore") else None
    if isinstance(ours, L.Overwrite):
        if name == "Overwrite":
            return "retryable"
        return None
    if isinstance(ours, L.Restore) or isinstance(ours, _ReserveFragments):
        if isinstance(ours, _ReserveFragments) and name in ("Overwrite", "Restore"):
            return "incompatible"
        return None
    if isinstance(ours, (L.Delete, L.Update)):
        if name in always_ok or name in ("CreateIndex", "ReserveFragments", "Project", "UpdateConfig", "Append",
                                         "DataOverlay"):
            return None
        if name in ("Overwrite", "Restore"):
            return "incompatible"
        if name == "Merge":
            return "retryable"
        return "retryable" if _touched(ours) & _touched(theirs) else None
    if isinstance(ours, L.Rewrite):
        if name in always_ok or name in ("Append", "ReserveFragments", "Project", "UpdateConfig", "CreateIndex"):
            return None
        if name in ("Overwrite", "Restore"):
            return "incompatible"
        if name == "Merge":
            return "retryable"
        return "retryable" if _touched(ours) & _touched(theirs) else None
    if isinstance(ours, L.Merge):
        if name in always_ok or name in ("ReserveFragments", "UpdateConfig", "CreateIndex"):
            return None
        if name in ("Overwrite", "Restore", "Project"):
            return "incompatible"
        return "retryable"
    if isinstance(ours, L.Project):
        if name in ("Merge", "Project"):
            return "retryable"
        if name in ("Overwrite", "Restore"):
            return "incompatible"
        return None
    if isinstance(ours, L.UpdateConfig):
        mine_up, mine_del = _upsert_keys(ours)
        if name == "Overwrite":
            if ours.schema_metadata_updates is not None or ours.field_metadata_updates:
                return "incompatible"
            return None
        if name == "UpdateConfig":
            their_up, their_del = _upsert_keys(theirs)
            if (mine_up | mine_del) & (their_up | their_del):
                return "incompatible"
            for attr in ("table_metadata_updates", "schema_metadata_updates"):
                a, b = getattr(ours, attr), getattr(theirs, attr)
                if a is not None and b is not None and (a.replace or b.replace or set(a.updates) & set(b.updates)):
                    return "incompatible"
            if set(ours.field_metadata_updates or {}) & set(theirs.field_metadata_updates or {}):
                return "incompatible"
        return None
    if isinstance(ours, L.DataReplacement):
        if name in always_ok or name in ("Append", "UpdateConfig", "ReserveFragments", "DataOverlay"):
            return None
        if name in ("Overwrite", "Restore"):
            return "incompatible"
        if name == "Merge":
            return "retryable"
        if isinstance(theirs, L.Delete):
            return "incompatible" if _touched(ours) & set(theirs.deleted_fragment_ids) else None
        if isinstance(theirs, L.Update):
            if _touched(ours) & set(theirs.removed_fragment_ids):
                return "incompatible"
            moved = bool(theirs.new_fragments) and (theirs.update_mode or "rewrite_rows") == "rewrite_rows"
            for g in ours.replacements:
                if any(f.id == g.fragment_id for f in theirs.updated_fragments):
                    if moved or any(x >= 0 and x in theirs.fields_modified for x in g.new_file.fields):
                        return "retryable"
            return None
        if isinstance(theirs, L.CreateIndex):
            depended = {x for idx in theirs.new_indices for x in idx.fields}
            return "retryable" if any(x in depended for g in ours.replacements for x in g.new_file.fields) else None
        if isinstance(theirs, L.Rewrite):
            return "retryable" if _touched(ours) & _touched(theirs) else None
        if isinstance(theirs, L.DataReplacement):
            for g in ours.replacements:
                for h in theirs.replacements:
                    if g.fragment_id == h.fragment_id and set(g.new_file.fields) & set(h.new_file.fields):
                        return "retryable"
            return None
        if isinstance(theirs, L.Project):
            return None
        return None
    return None
