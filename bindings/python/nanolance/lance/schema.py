# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor

"""``lance.schema``: a dataset's schema as Lance holds it -- fields with ids, a child naming its parent.

``LanceDataset.lance_schema`` returns a :class:`LanceSchema` read from the manifest;
:meth:`LanceSchema.from_pyarrow` numbers a pyarrow schema's fields as a new dataset would (pre-order,
from 0).
"""

from __future__ import annotations

import json
from typing import Any, Dict, List, Optional

import pyarrow as pa

from nanolance import _nanolance

__all__ = ["LanceField", "LanceSchema"]

_PK = "lance-schema:unenforced-primary-key"
_PK_POSITION = "lance-schema:unenforced-primary-key:position"
_CK_POSITION = "lance-schema:unenforced-clustering-key:position"
_ENCODINGS = {1: "Plain", 2: "VarBinary", 3: "Dictionary", 4: "RLE"}


def _rust_str(s: str) -> str:
    return json.dumps(s, ensure_ascii=False)


class LanceField:
    """A field of a :class:`LanceSchema`."""

    def __init__(self, info: Dict[str, Any], children: List["LanceField"], arrow: Optional[pa.Field] = None):
        self._info = info
        self._children = children
        self._arrow = arrow

    def name(self) -> str:
        return self._info["name"]

    def id(self) -> int:
        return int(self._info["id"])

    def children(self) -> List["LanceField"]:
        return list(self._children)

    @property
    def metadata(self) -> Dict[str, str]:
        return dict(self._info["metadata"])

    def to_arrow(self) -> pa.Field:
        if self._arrow is None:
            raise NotImplementedError("this field has no Arrow form here (a schema rebuilt from protos)")
        return self._arrow

    def unenforced_primary_key_position(self) -> Optional[int]:
        meta = self._info["metadata"]
        try:
            return int(meta[_PK_POSITION])
        except (KeyError, ValueError):
            return 0 if meta.get(_PK, "").lower() in ("1", "true", "yes", "on") else None

    def is_unenforced_primary_key(self) -> bool:
        return self.unenforced_primary_key_position() is not None

    def unenforced_clustering_key_position(self) -> Optional[int]:
        try:
            return int(self._info["metadata"][_CK_POSITION])
        except (KeyError, ValueError):
            return None

    def is_unenforced_clustering_key(self) -> bool:
        return self.unenforced_clustering_key_position() is not None

    def _key(self):
        i = self._info
        return (i["name"], int(i["id"]), int(i["parent_id"]), i["logical_type"], bool(i["nullable"]),
                tuple(sorted(i["metadata"].items())), tuple(c._key() for c in self._children))

    def __eq__(self, other) -> bool:
        return isinstance(other, LanceField) and self._key() == other._key()

    def __hash__(self) -> int:
        return hash(self._key())

    def __repr__(self) -> str:
        i = self._info
        meta = ", ".join(f"{_rust_str(k)}: {_rust_str(v)}" for k, v in sorted(i["metadata"].items()))
        encoding = _ENCODINGS.get(int(i.get("encoding", 0) or 0))

        def position(p):
            return "None" if p is None else f"Some({p})"

        return (f"Field {{ name: {_rust_str(i['name'])}, id: {i['id']}, parent_id: {i['parent_id']}, "
                f"logical_type: LogicalType({_rust_str(i['logical_type'])}), metadata: {{{meta}}}, "
                f"encoding: {'None' if encoding is None else f'Some({encoding})'}, "
                f"nullable: {'true' if i['nullable'] else 'false'}, "
                f"children: [{', '.join(repr(c) for c in self._children)}], dictionary: None, "
                f"unenforced_primary_key_position: {position(self._info['metadata'].get(_PK_POSITION))}, "
                f"unenforced_clustering_key_position: {position(self.unenforced_clustering_key_position())} }}")


class LanceSchema:
    """A Lance schema: its fields (with ids and children) and its metadata."""

    def __init__(self, fields: List[Dict[str, Any]], metadata: Optional[Dict[str, str]] = None,
                 arrow: Optional[pa.Schema] = None):
        self._infos = [dict(f) for f in fields]
        self._metadata = dict(metadata or {})
        self._arrow = arrow
        by_id: Dict[int, LanceField] = {}
        kids: Dict[int, List[Dict[str, Any]]] = {}
        for f in self._infos:
            kids.setdefault(int(f["parent_id"]), []).append(f)

        def build(info, arrow_field):
            children = []
            arrow_children = _arrow_children(arrow_field)
            for k, child in enumerate(kids.get(int(info["id"]), [])):
                children.append(build(child, arrow_children[k] if k < len(arrow_children) else None))
            field = LanceField(info, children, arrow_field)
            by_id[int(info["id"])] = field
            return field

        top = kids.get(-1, [])
        self._fields = [build(f, arrow.field(k) if arrow is not None and k < len(arrow) else None)
                        for k, f in enumerate(top)]

    # ── construction ──────────────────────────────────────────────────────────────────────────────

    @staticmethod
    def from_pyarrow(schema: pa.Schema) -> "LanceSchema":
        fields = _nanolance._lance_fields_from_arrow(schema)
        metadata = {k.decode(): v.decode() for k, v in (schema.metadata or {}).items()}
        return LanceSchema(fields, metadata, schema)

    @staticmethod
    def _from_protos(metadata_json: str, *field_protos: bytes) -> "LanceSchema":
        try:
            raw = json.loads(metadata_json)
            metadata = {k: bytes(v).decode() for k, v in raw.items()}
        except (TypeError, ValueError, AttributeError) as exc:
            raise ValueError(f"Failed to parse metadata: {exc}") from None
        fields: List[Dict[str, Any]] = []
        seen = set()
        for proto in field_protos:
            f = _decode_field(bytes(proto))
            if f["parent_id"] != -1 and f["parent_id"] not in seen:
                raise ValueError(
                    f"Failed to reconstruct schema: LanceError(Schema): Field '{f['name']}' (id={f['id']}) "
                    f"references parent id {f['parent_id']}, which must appear earlier in the protobuf field list")
            seen.add(f["id"])
            fields.append(f)
        return LanceSchema(fields, metadata)

    @staticmethod
    def _restore(fields, metadata, arrow) -> "LanceSchema":
        return LanceSchema(fields, metadata, arrow)

    def __reduce__(self):
        return (LanceSchema._restore, (self._infos, self._metadata, self._arrow))

    # ── access ────────────────────────────────────────────────────────────────────────────────────

    def fields(self) -> List[LanceField]:
        return list(self._fields)

    def _resolve(self, name: str, fold: bool) -> Optional[LanceField]:
        from nanolance.lance.dataset import _path_parts

        parts = _path_parts(name)
        level = self._fields
        found = None
        for part in parts:
            match = next((f for f in level if f.name() == part), None)
            if match is None and fold:
                match = next((f for f in level if f.name().lower() == part.lower()), None)
            if match is None:
                return None
            found, level = match, match.children()
        return found

    def field(self, name: str) -> Optional[LanceField]:
        return self._resolve(name, False)

    def field_case_insensitive(self, name: str) -> Optional[LanceField]:
        return self._resolve(name, True)

    def _pre_order(self) -> List[LanceField]:
        out: List[LanceField] = []

        def walk(fields):
            for f in fields:
                out.append(f)
                walk(f.children())

        walk(self._fields)
        return out

    def unenforced_primary_key(self) -> List[LanceField]:
        keys = [f for f in self._pre_order() if f.is_unenforced_primary_key()]
        return sorted(keys, key=lambda f: (False, f.unenforced_primary_key_position(), f.id())
                      if f.unenforced_primary_key_position() else (True, f.id(), f.id()))

    def unenforced_clustering_key(self) -> List[LanceField]:
        keys = [f for f in self._pre_order() if f.is_unenforced_clustering_key()]
        return sorted(keys, key=lambda f: (f.unenforced_clustering_key_position(), f.id()))

    def to_pyarrow(self) -> pa.Schema:
        if self._arrow is None:
            raise NotImplementedError("this schema has no Arrow form here (a schema rebuilt from protos)")
        return self._arrow

    def __eq__(self, other) -> bool:
        return (isinstance(other, LanceSchema) and self._metadata == other._metadata
                and [f._key() for f in self._fields] == [f._key() for f in other._fields])

    def __hash__(self) -> int:
        return hash(tuple(f._key() for f in self._fields))

    def __repr__(self) -> str:
        meta = ", ".join(f"{_rust_str(k)}: {_rust_str(v)}" for k, v in sorted(self._metadata.items()))
        return f"Schema {{ fields: [{', '.join(repr(f) for f in self._fields)}], metadata: {{{meta}}} }}"


def _arrow_children(field: Optional[pa.Field]) -> List[pa.Field]:
    if field is None:
        return []
    t = field.type
    if pa.types.is_struct(t):
        return [t.field(k) for k in range(t.num_fields)]
    if pa.types.is_list(t) or pa.types.is_large_list(t) or pa.types.is_list_view(t):
        return [t.value_field]
    if pa.types.is_map(t):
        return [pa.field("entries", pa.struct([t.key_field, t.item_field]), False)]
    return []


def _decode_field(data: bytes) -> Dict[str, Any]:
    """A Lance ``file.Field`` protobuf message: the parts a LanceSchema holds."""
    out: Dict[str, Any] = {"name": "", "id": 0, "parent_id": 0, "logical_type": "", "nullable": False,
                           "metadata": {}, "encoding": 0}
    pos = 0

    def varint():
        nonlocal pos
        shift = value = 0
        while True:
            if pos >= len(data):
                raise ValueError("Failed to decode field: truncated message")
            b = data[pos]
            pos += 1
            value |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return value

    def signed32(v):
        v &= 0xFFFFFFFF
        return v - (1 << 32) if v & 0x80000000 else v

    while pos < len(data):
        key = varint()
        number, wire = key >> 3, key & 7
        if wire == 0:
            v = varint()
            if number == 3:
                out["id"] = signed32(v)
            elif number == 4:
                out["parent_id"] = signed32(v)
            elif number == 6:
                out["nullable"] = bool(v)
            elif number == 7:
                out["encoding"] = v
        elif wire == 2:
            n = varint()
            chunk = data[pos:pos + n]
            pos += n
            if number == 2:
                out["name"] = chunk.decode()
            elif number == 5:
                out["logical_type"] = chunk.decode()
            elif number == 10:
                entry = _decode_map_entry(chunk)
                out["metadata"][entry[0]] = entry[1]
        elif wire == 5:
            pos += 4
        elif wire == 1:
            pos += 8
        else:
            raise ValueError("Failed to decode field: unsupported wire type")
    return out


def _decode_map_entry(data: bytes):
    key, value, pos = "", "", 0
    while pos < len(data):
        tag = data[pos]
        pos += 1
        n = shift = 0
        while True:
            b = data[pos]
            pos += 1
            n |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
        chunk = data[pos:pos + n]
        pos += n
        if tag >> 3 == 1:
            key = chunk.decode()
        else:
            value = chunk.decode(errors="replace")
    return key, value


def __getattr__(name: str):
    from nanolance.lance._stubs import placeholder

    if name.startswith("__"):
        raise AttributeError(name)
    return placeholder(f"lance.schema.{name}")
