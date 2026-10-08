# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor

"""Transactions as pylance presents them: ``LanceOperation.*`` and ``Transaction``, read from the
``_transactions/*.txn`` file a version's manifest names (lance.table.Transaction, protobuf).

Reading only: committing a hand-built transaction (``LanceDataset.commit``) is not implemented.
"""

from __future__ import annotations

import dataclasses
import struct
import uuid as _uuid
from abc import ABC
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Dict, Iterator, List, Optional, Set, Tuple

from nanolance.lance.fragment import DataFile, DeletionFile, FragmentMetadata

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


class LanceOperation:
    class BaseOperation(ABC):
        """An operation a transaction applies."""

    @dataclass
    class Overwrite(BaseOperation):
        new_schema: Any
        fragments: List[FragmentMetadata]
        initial_bases: Optional[list] = None

    @dataclass
    class Append(BaseOperation):
        fragments: List[FragmentMetadata]

    @dataclass
    class Delete(BaseOperation):
        updated_fragments: List[FragmentMetadata]
        deleted_fragment_ids: List[int]
        predicate: str

    @dataclass
    class Update(BaseOperation):
        removed_fragment_ids: List[int] = dataclasses.field(default_factory=list)
        updated_fragments: List[FragmentMetadata] = dataclasses.field(default_factory=list)
        new_fragments: List[FragmentMetadata] = dataclasses.field(default_factory=list)
        fields_modified: List[int] = dataclasses.field(default_factory=list)
        fields_for_preserving_frag_bitmap: List[int] = dataclasses.field(default_factory=list)
        update_mode: str = ""
        updated_fragment_offsets: Optional[Dict[int, bytes]] = None

    @dataclass
    class Merge(BaseOperation):
        fragments: List[FragmentMetadata]
        schema: Any
        preserves_nullability: bool = False

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
            f.path = value.decode()
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
    deletion = None
    for number, _, value in _fields(data):
        if number == 1:
            fragment.id = value
        elif number == 2:
            fragment.files.append(_data_file(value))
        elif number == 3:
            deletion = {n: v for n, _, v in _fields(value)}
        elif number == 4:
            fragment.physical_rows = value
    if deletion is not None:
        suffix = "bin" if deletion.get(1, 0) == 1 else "arrow"
        fragment.deletion_file = DeletionFile(
            path=f"_deletions/{fragment.id}-{deletion.get(2, 0)}-{deletion.get(3, 0)}.{suffix}",
            num_deleted_rows=deletion.get(4, 0))
    return fragment


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
    from nanolance.lance.schema import LanceSchema, _decode_field

    fields = [_decode_field(bytes(p)) for p in field_protos]
    return LanceSchema(fields, {k: v.decode(errors="replace") for k, v in metadata.items()})


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
        return LanceOperation.BaseOperation.__new__(_ReserveFragments)
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
    return LanceOperation.BaseOperation.__new__(_OtherOperation)


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
