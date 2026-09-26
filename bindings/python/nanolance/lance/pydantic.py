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
    import typing

    simple = {int: pa.int64(), float: pa.float64(), str: pa.utf8(), bool: pa.bool_(), bytes: pa.binary(),
              datetime.datetime: pa.timestamp("us"), datetime.date: pa.date32()}

    def arrow_type(tp):
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
        raise TypeError(f"no Arrow type for {tp!r}")

    fields = []
    for name, f in model.model_fields.items():
        typ, optional = arrow_type(f.annotation)
        fields.append(pa.field(name, typ, nullable=optional))
    return pa.schema(fields)

def __getattr__(name: str):
    """pylance names this module does not implement import as placeholders (see _stubs.py)."""
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.pydantic.{name}")
    globals()[name] = value
    return value
