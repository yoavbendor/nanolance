# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Writing lists of pydantic models, as ``lance.write_dataset`` accepts them."""

from __future__ import annotations

import datetime
import enum
import inspect
import sys
import types
import typing
from typing import Any, Union

import pyarrow as pa

try:
    import pydantic
except ImportError:  # pydantic is optional; only the functions below need it
    pydantic = None


class FixedSizeListMixin:
    """Marks the Vector / MultiVector types: their ``dim()``, ``value_arrow_type()``, ``nullable()``."""


def Vector(dim: int, value_type: pa.DataType = pa.float32(), nullable: bool = True):
    """Pydantic type for a fixed-size-list embedding vector column (``list<value_type>[dim]``)."""

    class FixedSizeList(list, FixedSizeListMixin):
        def __repr__(self):
            return f"FixedSizeList(dim={dim})"

        @staticmethod
        def nullable() -> bool:
            return nullable

        @staticmethod
        def dim() -> int:
            return dim

        @staticmethod
        def value_arrow_type() -> pa.DataType:
            return value_type

        @classmethod
        def __get_pydantic_core_schema__(cls, _source_type, _handler):
            from pydantic_core import core_schema

            return core_schema.no_info_after_validator_function(
                cls,
                core_schema.list_schema(min_length=dim, max_length=dim, items_schema=core_schema.float_schema()),
            )

    return FixedSizeList


def MultiVector(dim: int, value_type: pa.DataType = pa.float32(), nullable: bool = True):
    """Pydantic type for a list of fixed-size-list embedding vectors (``list<list<value_type>[dim]>``)."""

    class MultiVectorList(list, FixedSizeListMixin):
        def __repr__(self):
            return f"MultiVector(dim={dim})"

        @staticmethod
        def nullable() -> bool:
            return nullable

        @staticmethod
        def dim() -> int:
            return dim

        @staticmethod
        def value_arrow_type() -> pa.DataType:
            return value_type

        @staticmethod
        def is_multi_vector() -> bool:
            return True

        @classmethod
        def __get_pydantic_core_schema__(cls, _source_type, _handler):
            from pydantic_core import core_schema

            return core_schema.no_info_after_validator_function(
                cls,
                core_schema.list_schema(
                    items_schema=core_schema.list_schema(
                        min_length=dim, max_length=dim, items_schema=core_schema.float_schema()
                    ),
                ),
            )

    return MultiVectorList


def _pydantic_reader(items, schema=None, model=None) -> pa.RecordBatchReader:
    model = model or type(items[0])
    for i, item in enumerate(items):
        if type(item) is not model:
            raise TypeError(f"data[{i}] is not exactly an instance of {model.__name__}")
    if schema is None:
        schema = pydantic_to_schema(model)
    rows = [item.model_dump() for item in items]
    batch = pa.RecordBatch.from_pylist(rows, schema=schema)
    return pa.RecordBatchReader.from_batches(batch.schema, [batch])


def _field_annotation(field) -> Any:
    """The annotation of a pydantic v2 FieldInfo, or of a pydantic v1 ModelField (``outer_type_``)."""
    return field.annotation if hasattr(field, "annotation") else field.outer_type_


def _extra(field, key: str):
    """A value of the field's ``json_schema_extra`` (where ``Field(json_schema_extra={...})`` puts it)."""
    if hasattr(field, "json_schema_extra"):
        return (field.json_schema_extra or {}).get(key)
    return (field.field_info.extra or {}).get("json_schema_extra", {}).get(key)


def _is_subclass(tp, base) -> bool:
    return inspect.isclass(tp) and issubclass(tp, base)


def _unwrap_optional(annotation):
    """``X`` for ``Optional[X]`` / ``X | None``; None when the annotation is not that."""
    origin = typing.get_origin(annotation)
    if origin is Union or (sys.version_info >= (3, 10) and isinstance(annotation, types.UnionType)):
        args = typing.get_args(annotation)
        non_none = [a for a in args if a is not type(None)]
        if len(non_none) == 1 and len(non_none) != len(args):
            return non_none[0]
    return None


def _pydantic_type_to_arrow_type(tp, field) -> pa.DataType:
    """The Arrow type of an annotation: an ``Optional`` is its inner type (nullability is `is_nullable`)."""
    unwrapped = _unwrap_optional(tp)
    if unwrapped is not None:
        tp = unwrapped
    if pydantic is not None and _is_subclass(tp, pydantic.BaseModel):
        return pa.struct(_pydantic_model_to_fields(tp))
    if _is_subclass(tp, FixedSizeListMixin):
        if getattr(tp, "is_multi_vector", lambda: False)():
            return pa.list_(pa.list_(tp.value_arrow_type(), tp.dim()))
        return pa.list_(tp.value_arrow_type(), tp.dim())
    if _is_subclass(tp, enum.Enum):
        # As pylance: a string enum is dictionary-encoded, an enum of one other type takes that
        # type, a mixed or empty one is utf8.
        kinds = {type(m.value) for m in tp}
        if len(kinds) == 1:
            kind = kinds.pop()
            if kind is str:
                return pa.dictionary(pa.int32(), pa.utf8())
            return _pydantic_type_to_arrow_type(kind, field)
        return pa.utf8()
    if tp is int:
        return pa.int64()
    if tp is float:
        return pa.float64()
    if tp is str:
        return pa.utf8()
    if tp is bool:
        return pa.bool_()
    if tp is bytes:
        return pa.binary()
    if tp is datetime.date:
        return pa.date32()
    if tp is datetime.datetime:
        return pa.timestamp("us", tz=_extra(field, "tz"))
    if typing.get_origin(tp) in (list, tuple):
        args = typing.get_args(tp)
        if not args:
            raise TypeError(
                "Converting Pydantic type to Arrow Type: unsupported type "
                f"{tp}. Specify the element type, e.g. List[int] instead of a bare List."
            )
        return pa.list_(_pydantic_type_to_arrow_type(args[0], field))
    raise TypeError(f"Converting Pydantic type to Arrow Type: unsupported type {tp}.")


def is_nullable(field) -> bool:
    """Whether a pydantic field is nullable: only an ``Optional`` annotation or a nullable Vector /
    MultiVector makes it so. A plain default value (``count: int = 0``) does not."""
    if not hasattr(field, "annotation"):  # pydantic v1 ModelField: Optional-ness is `allow_none`
        annotation = _field_annotation(field)
        if _is_subclass(annotation, FixedSizeListMixin):
            return annotation.nullable()
        return bool(field.allow_none)
    annotation = field.annotation
    if _unwrap_optional(annotation) is not None:
        return True
    if typing.get_origin(annotation) is Union or (
        sys.version_info >= (3, 10) and isinstance(annotation, types.UnionType)
    ):
        return type(None) in typing.get_args(annotation)
    if _is_subclass(annotation, FixedSizeListMixin):
        return annotation.nullable()
    return False


def _pydantic_model_to_fields(model) -> list:
    fields = model.model_fields if hasattr(model, "model_fields") else model.__fields__
    return [pa.field(name, _pydantic_type_to_arrow_type(_field_annotation(f), f), is_nullable(f))
            for name, f in fields.items()]


def pydantic_to_schema(model) -> pa.Schema:
    """The Arrow schema of a pydantic model's fields: nested models are structs, string enums are
    dictionary-encoded, ``datetime`` takes its ``json_schema_extra={"tz": ...}`` timezone, and
    ``Vector`` / ``MultiVector`` are fixed-size lists."""
    return pa.schema(_pydantic_model_to_fields(model))


def __getattr__(name: str):
    """pylance names this module does not implement import as placeholders (see _stubs.py)."""
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.pydantic.{name}")
    globals()[name] = value
    return value
