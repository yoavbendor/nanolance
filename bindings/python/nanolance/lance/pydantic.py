# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Writing lists of pydantic models, as ``lance.write_dataset`` accepts them."""

from __future__ import annotations

import pyarrow as pa


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


def pydantic_to_schema(model) -> pa.Schema:
    """The Arrow schema of a pydantic model's fields (required fields are not nullable)."""
    import datetime
    import enum
    import typing

    simple = {int: pa.int64(), float: pa.float64(), str: pa.utf8(), bool: pa.bool_(), bytes: pa.binary(),
              datetime.datetime: pa.timestamp("us"), datetime.date: pa.date32()}

    def arrow_type(tp):
        if isinstance(tp, type) and "_nl_arrow" in vars(tp):  # MultiVector(dim): its own Arrow type
            return tp._nl_arrow, tp._nl_nullable
        origin = typing.get_origin(tp)
        args = typing.get_args(tp)
        if origin is typing.Union or (origin is not None and str(origin) == "types.UnionType"):
            inner = [a for a in args if a is not type(None)]
            return arrow_type(inner[0])[0], True
        if origin in (list, typing.List):
            return pa.list_(arrow_type(args[0])[0]), False
        if tp in simple:
            return simple[tp], False
        if hasattr(tp, "model_fields"):
            return pa.struct([pa.field(n, *arrow_type(f.annotation)) for n, f in tp.model_fields.items()]), False
        if isinstance(tp, type) and issubclass(tp, enum.Enum):
            # As pylance: a string enum is dictionary-encoded, an enum of one other type takes that
            # type, a mixed or empty one is utf8.
            kinds = {type(m.value) for m in tp}
            if len(kinds) == 1:
                kind = kinds.pop()
                if kind is str:
                    return pa.dictionary(pa.int32(), pa.utf8()), False
                return arrow_type(kind)[0], False
            return pa.utf8(), False
        raise TypeError(f"no Arrow type for {tp!r}")

    fields = []
    for name, f in model.model_fields.items():
        typ, optional = arrow_type(f.annotation)
        fields.append(pa.field(name, typ, nullable=optional))
    return pa.schema(fields)

def MultiVector(dim: int, value_type: pa.DataType = pa.float32(), nullable: bool = True):
    """Pydantic type for a list of fixed-size-list embedding vectors (ColBERT-style multivectors),
    as pylance's ``lance.pydantic.MultiVector``: the field is ``list<fixed_size_list<value_type, dim>>``,
    nullable unless ``nullable=False``."""
    if int(dim) <= 0:
        raise ValueError(f"MultiVector dimension must be positive, got {dim}")

    class MultiVectorList(list):
        _nl_arrow = pa.list_(pa.list_(value_type, int(dim)))
        _nl_nullable = bool(nullable)

        def __repr__(self):
            return f"MultiVector(dim={dim})"

        @classmethod
        def __get_pydantic_core_schema__(cls, source, handler):  # pydantic validates it as a list of lists
            from pydantic_core import core_schema

            return core_schema.list_schema(core_schema.list_schema(core_schema.float_schema()))

    return MultiVectorList


def __getattr__(name: str):
    """pylance names this module does not implement import as placeholders (see _stubs.py)."""
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.pydantic.{name}")
    globals()[name] = value
    return value
